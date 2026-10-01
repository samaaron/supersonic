#!/bin/bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2025-2026 Sam Aaron
#
# build-web.sh — SuperSonic compiled for the AudioWorklet.
#
# Clockwork plus the scsynth guest, the same two halves the native build
# links, emitted as scsynth-nrt.wasm — which is the name the JS client and the
# Playwright suite both expect. The module is built by the same CMake tree as
# the native process (`emcmake cmake -B build/web`); this script wraps that
# and bundles the JavaScript around it.
#
# The web target is the same clockwork as the native one: the same C++ core, the
# same Rust subsystems, the same DSP seam. What changes is which subsystems are
# present. midir, gilrs and std::net have no worklet to run in, so clockwork's
# umbrella crate is built with those features off — the SAME crate, not a
# web-only fork of it, which is why a subsystem cannot drift between the two
# targets. The guest is C++ throughout and brings no Rust of its own.
#
# Output is a STANDALONE_WASM module with imported shared memory: the worklet
# supplies the memory, so JS and clockwork address one heap.
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
CLOCKWORK="$ROOT/clockwork"
GUEST="$ROOT/dsp/scsynth"
OUT="$ROOT/dist/wasm"

DSP="scsynth"
SCHEDULER="${CLOCKWORK_SCHEDULER:-1}"
OPT="${CLOCKWORK_OPT:--O3}"
# The JavaScript half's build mode. A development build keeps __DEV__ true
# (the client's console diagnostics stay in) and the bundles readable; a
# release build (--release, what CI and npm publish use) compiles the
# diagnostics out and minifies, with a source map beside each bundle.
JS_DEV=true
JS_MINIFY=""

for arg in "$@"; do
    case "$arg" in
        --no-scheduler) SCHEDULER=0 ;;
        --dsp=*)        DSP="${arg#--dsp=}" ;;
        --debug)        OPT="-O0 -g" ;;
        --release)      OPT="-O3"; JS_DEV=false; JS_MINIFY="--minify --sourcemap" ;;
        *) echo "unknown argument: $arg" >&2; exit 1 ;;
    esac
done

command -v emcc >/dev/null || {
    echo "emcc not found. Run: source <emsdk>/emsdk_env.sh" >&2; exit 1; }

# emsdk ships its own node; prefer it so the build does not depend on a system
# node that may not exist (this box has none on PATH).
NODE="$(command -v node || ls -d "${EMSDK:-/nonexistent}"/node/*/bin/node 2>/dev/null | head -1)"
# The version the engine says under its banner: package.json's, as the npm packages and the client say it.
PRODUCT_VERSION="$("$NODE" -p "require('$ROOT/package.json').version")"
[ -x "$NODE" ] || { echo "no node found for the memory config reader" >&2; exit 1; }

mkdir -p "$OUT"

# ── The module: clockwork's CMake, for the wasm target ───────────────────────
# One tree, one list of sources, one set of flags: the same CMake that builds
# the native process configures the module when run under emcmake. What is
# SuperSonic's own is set in CMakeLists.txt (the module's name, the
# scheduler's shape); everything else — the exports, the memory sizes read
# from js/memory_layout.js, the nightly Rust for the wasm target — is
# clockwork's to know, in clockwork/CMakeLists.txt.
BUILD_DIR="$ROOT/build/web"
CMAKE_BUILD_TYPE=Release
[ "$OPT" = "-O0 -g" ] && CMAKE_BUILD_TYPE=Debug
echo "SuperSonic → wasm   dsp=$DSP scheduler=$SCHEDULER ($CMAKE_BUILD_TYPE)"
emcc --version | head -1
mkdir -p "$ROOT/build"   # the configure log goes beside the build dir, on a fresh checkout too
emcmake cmake -B "$BUILD_DIR" -S "$ROOT" \
    -DCMAKE_BUILD_TYPE="$CMAKE_BUILD_TYPE" \
    -DCLOCKWORK_SCHEDULER="$([ "$SCHEDULER" = 1 ] && echo ON || echo OFF)" \
    -DCLOCKWORK_PRODUCT_VERSION="$PRODUCT_VERSION" \
    -DCLOCKWORK_WEB_AUDIO_FILES="$([ "${CLOCKWORK_WEB_AUDIO_FILES:-0}" = 1 ] && echo ON || echo OFF)" \
    -DCLOCKWORK_WEB_ASSERTIONS="${CLOCKWORK_ASSERTIONS:-0}" \
    -DCLOCKWORK_WEB_SAFE_HEAP="${CLOCKWORK_SAFE_HEAP:-0}" \
    -DCLOCKWORK_WEB_KEEP_NAMES="$([ -n "${CLOCKWORK_KEEP_NAMES:-}" ] && echo ON || echo OFF)" \
    ${CLOCKWORK_RUST_NIGHTLY:+-DCLOCKWORK_RUST_NIGHTLY="$CLOCKWORK_RUST_NIGHTLY"} \
    > "$BUILD_DIR.configure.log" 2>&1 || { cat "$BUILD_DIR.configure.log"; exit 1; }
cmake --build "$BUILD_DIR" --parallel

# ── The main-thread subsystems (MIDI, gamepad) ───────────────────────────────
# Rust cores compiled to wasm-bindgen modules for the browser. They are
# clockwork's — its js/lib/{midi,gamepad}_manager.js import their glue by
# relative path from clockwork's dist — so clockwork's script builds them into
# clockwork's dist, and it runs BEFORE the bundle below, which imports them.
"$CLOCKWORK/scripts/build-web-subsystems.sh"

# ─── The JavaScript half ─────────────────────────────────────────────────────
# The wasm is useless on its own: something has to load it, feed it blocks and
# carry OSC. That is js/, bundled here into the dist/ layout the client and the
# test fixtures both expect.
JS="$CLOCKWORK/js"
ESBUILD="${ESBUILD:-$ROOT/node_modules/.bin/esbuild}"
# A global esbuild will do when the local one is absent (CI images carry one).
[ -x "$ESBUILD" ] || ESBUILD="$(command -v esbuild || echo "$ESBUILD")"
# Where to resolve npm dependencies from. Clockwork's JS uses @thi.ng/malloc
# for the buffer pool; this repository's package.json declares it, so a plain
# `npm install` at the root is all that is owed.
NODE_PATHS="${NODE_PATHS:-$ROOT/node_modules}"
if [ ! -x "$ESBUILD" ]; then
    echo "esbuild not found at $ESBUILD — run 'npm install' first, or set ESBUILD=..." >&2
    exit 1
fi

export NODE_PATH="$NODE_PATHS"
echo "bundling the client and workers..."
# The client. --external on the wasm keeps esbuild from trying to inline it.
# SuperSonic's client — clockwork's class plus what scsynth means — is the one
# entry point; clockwork is absorbed into it, not offered beside it.
# --splitting: what the client imports on demand (clockwork's MIDI and
# gamepad managers, with their wasm-bindgen glue) becomes a chunk under
# dist/chunks/, fetched only by a page that enables them.
rm -rf "$OUT/../chunks"
"$ESBUILD" "$ROOT/js/supersonic.js" --bundle --format=esm --splitting --define:__DEV__=$JS_DEV $JS_MINIFY \
    --outdir="$OUT/.." --chunk-names="chunks/[name]-[hash]" --external:./scsynth-nrt.wasm >/dev/null
rm -f "$OUT/../clockwork.js"

for entry in osc_channel.js lib/osc_fast.js lib/osc_in_pump.js lib/midi_event.js lib/metrics_component.js; do
    "$ESBUILD" "$JS/$entry" --bundle --format=esm --define:__DEV__=$JS_DEV $JS_MINIFY \
        --outfile="$OUT/../$(basename "$entry")" >/dev/null
done
cp "$JS"/lib/metrics-*.css "$OUT/.." 2>/dev/null || true

# Every module and stylesheet at dist/'s top level is the client package's, so package.json's "files" must name
# it: 0.84.1 was published without osc_in_pump.js and midi_event.js, and a page importing them from the CDN got
# a 404. Checked here because `npm publish` runs this build first (prepublishOnly).
"$NODE" -e '
  const fs = require("fs");
  const [dist, pkg] = process.argv.slice(1);
  const listed = new Set(require(pkg).files);
  const missing = fs.readdirSync(dist).filter((f) => /\.(js|css)$/.test(f)).map((f) => `dist/${f}`).filter((f) => !listed.has(f));
  if (missing.length) { console.error(`package.json "files" leaves out ${missing.join(", ")}: add them, or npm ships without them`); process.exit(1); }
' "$OUT/.." "$ROOT/package.json"

# Workers are iife: a worker has no module loader to hand them to.
rm -rf "$OUT/../workers"; mkdir -p "$OUT/../workers"
for worker in "$JS/workers/"*.js; do
    "$ESBUILD" "$worker" --bundle --format=iife --define:__DEV__=$JS_DEV $JS_MINIFY \
        --outfile="$OUT/../workers/$(basename "$worker")" >/dev/null
done

# The subsystem modules ride along in dist/ so the exported site and the
# distribution zip carry them; the client imports their glue from clockwork's
# own dist at bundle time. Their wasm goes into dist/wasm/ — and from there
# into the core package — because that is where the client looks for it:
# wasmBaseURL + clockwork_midi_bg.wasm (host_front.js). 0.80.0 shipped
# without them, and a page enabling MIDI or gamepad from the CDN got a 404.
for sub in midi gamepad; do
    rm -rf "$OUT/../$sub"
    [ -d "$ROOT/clockwork/dist/$sub" ] && cp -r "$ROOT/clockwork/dist/$sub" "$OUT/../$sub"
    [ -f "$ROOT/clockwork/dist/$sub/clockwork_${sub}_bg.wasm" ] \
        && cp "$ROOT/clockwork/dist/$sub/clockwork_${sub}_bg.wasm" "$OUT/"
done

# Assets the suite loads by URL.
rm -rf "$OUT/../synthdefs" "$OUT/../samples"
cp -r "$ROOT/packages/supersonic-scsynth-synthdefs/synthdefs" "$OUT/../synthdefs"
cp -r "$ROOT/packages/supersonic-scsynth-samples/samples"     "$OUT/../samples"

# ── npm packages ─────────────────────────────────────────────────────────────
# supersonic-scsynth-core ships the engine and the worklet: the copyleft half,
# published separately so the client package can be taken on its own.
CORE_PKG_DIR="$ROOT/packages/supersonic-scsynth-core"
rm -rf "$CORE_PKG_DIR/wasm" "$CORE_PKG_DIR/workers"
cp -r "$OUT" "$CORE_PKG_DIR/wasm"
mkdir -p "$CORE_PKG_DIR/workers"
cp "$OUT/../workers/clockwork_audio_worklet.js" "$CORE_PKG_DIR/workers/"

# The npm README is the docs folded into one file, so the package page has no
# dead links into docs/. Generated, not tracked (see .gitignore).
if [ -f "$ROOT/docs/API.md" ]; then
    "$NODE" "$ROOT/scripts/build-npm-readme.mjs"
fi

echo ""
echo "built $OUT/scsynth-nrt.wasm ($(du -h "$OUT/scsynth-nrt.wasm" | cut -f1))"
