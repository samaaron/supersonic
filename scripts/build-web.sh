#!/bin/bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2025-2026 Sam Aaron
#
# build-web.sh — SuperSonic compiled for the AudioWorklet.
#
# Clockwork plus the scsynth guest, the same two halves the native build
# links, emitted as scsynth-nrt.wasm — which is the name the JS client and the
# Playwright suite both expect.
#
# The web target is the same clockwork as the native one: the same C++ core, the
# same Rust subsystems, the same DSP seam. What changes is which subsystems are
# present. midir, gilrs and std::net have no worklet to run in, so the umbrella
# crate is built with those features off — the SAME crate, not a web-only fork
# of it, which is why a subsystem cannot drift between the two targets.
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

# The engine, the same three groups the CMake build compiles.
SCSYNTH_SOURCES=$(find "$GUEST/synth/server" "$GUEST/synth/common" "$GUEST/synth/plugins" \
    -name "*.cpp" ! -name "SC_Filesystem_win.cpp" ! -name "SC_Filesystem_macos.cpp" \
    ! -name "SC_Filesystem_iphone.cpp" 2>/dev/null | tr '\n' ' ')

# The C half. fftlib.c is the Green FFT the ugens link against — missed by a
# *.cpp sweep, and the link says so: undefined rffts / riffts.
SCSYNTH_C_SOURCES=$(find "$GUEST/synth/common" -name "*.c" 2>/dev/null | tr '\n' ' ')
SCSYNTH_C_SOURCES="$SCSYNTH_C_SOURCES $GUEST/synth/external_libraries/TLSF-2.4.6/src/tlsf.c"

mkdir -p "$OUT"

# Overload shedding is OFF (-DSCHEDULER_SHED_LATE_MS=0 below).
#
# It was 1000ms: an event more than a second late was dropped rather than
# played. That protects a flooded engine, but it contradicts what OSC says and
# what every SuperCollider client assumes — a bundle whose timetag has passed
# executes IMMEDIATELY, it does not disappear. A client that stalls for a
# second and then catches up expects its notes late, not silence.
#
# The policy is still in clockwork and a host that wants it can set this. If
# it comes back it wants a threshold far above any legitimate lateness, not one
# a single slow frame can cross.
#
# NOTE: this comment is HERE and not among the flags. A '#' inside a
# backslash-continued argument list comments out every flag after it, which is
# how NO_LIBSNDFILE once went missing with the line looking untouched.
echo "SuperSonic → wasm   dsp=$DSP scheduler=$SCHEDULER"
emcc --version | head -1

# Build-time and runtime memory come from ONE file. js/memory_layout.js is what
# the worklet allocates against; reading it here is what stops the module and
# its heap disagreeing about how big the heap is.
INITIAL_MEMORY=$("$NODE" "$CLOCKWORK/scripts/get_memory_config.js" initial)
MAX_MEMORY=$("$NODE" "$CLOCKWORK/scripts/get_memory_config.js" max)
# The placement arena clockwork::mem allocates from on this target. Read from the
# same file for the same reason as the two above: the module claims it at
# init_memory out of the gap the layout reserved, so a build that disagrees
# with the layout pushes that malloc into the guest's region.
MEM_ARENA_SIZE=$("$NODE" "$CLOCKWORK/scripts/get_memory_config.js" arena)
echo "memory: $((INITIAL_MEMORY / 1024 / 1024))MB initial, $((MAX_MEMORY / 1024 / 1024))MB ceiling, $((MEM_ARENA_SIZE / 1024 / 1024))MB arena"

# 1MB, not 4MB. The pool holds queued OSC payloads: 8192 slots of a typical
# ~72-byte message is well under 600KB, so 4MB was never sized from anything.
# It came out of the malloc budget — the gap before guest memory, which the
# static data has already mostly taken — and left ~2MB for the engine's own
# allocations. rt_pool_isolation, which asks for a 128MB RT pool and uses
# nearly all of it, crashed the renderer once the scheduler began actually
# allocating rather than silently failing.
SCHEDULER_DATA_POOL_SIZE=${SCHEDULER_DATA_POOL_SIZE:-$((512 * 1024))}
# 2048 slots and a 512KB pool, not 8192 and 4MB.
#
# The queue lives in a static arena inside the ~14MB gap before guest memory,
# which also holds the static data (7.4MB, mostly ring buffers) and the wasm
# stack. At 8192 slots the arena was 2MB of that, and the gap cannot afford it:
# with 2MB gone, 1000 concurrent synths crashed the renderer, and putting the
# arena on the heap instead simply moved the same 2MB and broke a different
# test. 2048 pending events with a 512KB payload pool is 772KB and ample —
# 8192 was never sized from anything.
SCHEDULER_SLOT_COUNT=${SCHEDULER_SLOT_COUNT:-2048}
CLOCKWORK_IN_BUFFER_SIZE=${CLOCKWORK_IN_BUFFER_SIZE:-$((1024 * 1024))}
WASM_STACK_SIZE=${WASM_STACK_SIZE:-1048576}

# ── Rust ─────────────────────────────────────────────────────────────────────
# One staticlib, because each carries its own std and two would collide at
# link time. `schedule` is the only feature the web build keeps: midi, gamepad
# and osc are native subsystems whose crates cannot run here.
#
# Shared memory needs the atomics and bulk-memory target features, and the
# precompiled std for this target has neither — hence -Z build-std, hence
# nightly. Without it the link fails with "--shared-memory is disallowed".
RUST_FEATURES=()
[ "$SCHEDULER" = "1" ] && RUST_FEATURES+=(schedule)
FEATURE_ARG=""
[ ${#RUST_FEATURES[@]} -gt 0 ] && FEATURE_ARG="--features $(IFS=,; echo "${RUST_FEATURES[*]}")"

# SuperSonic's umbrella, not clockwork's: only one Rust staticlib may be
# linked, and this one carries both halves. --no-default-features drops the
# native subsystems (midir, gilrs, std::net) that have no worklet to run in.
#
# FINDING THE NIGHTLY TOOLCHAIN, WITHOUT ASSUMING HOW RUST WAS INSTALLED.
#
# `cargo +nightly` only parses when cargo on PATH is one of rustup's proxies.
# Where cargo is a real binary — Homebrew's rust, a distro package — the
# directive is not a flag at all: "no such command: +nightly".
#
# `rustup run nightly cargo` is not enough either, and it fails in a way worth
# spelling out because the error names the wrong culprit. It does select the
# right CARGO, but cargo then spawns the compiler as bare `rustc`, resolved
# from PATH — which still finds the stable one when that comes first. std
# itself builds, and then every workspace crate dies on "the option `Z` is only
# accepted on the nightly compiler", which reads like no toolchain was selected
# when in fact only half of one was.
#
# So resolve both halves to absolute paths and pass RUSTC explicitly. That is
# immune to PATH order and to which installer put rust on the box.
# WHICH nightly. Pinned, because "nightly" is a moving target and -Z build-std
# is exactly the kind of unstable surface that changes under you: a green build
# and a broken one can differ only by the day someone last ran rustup update.
# Override with CLOCKWORK_RUST_NIGHTLY=nightly-YYYY-MM-DD, or "nightly" to float.
#
# The pin is a PREFERENCE, not a requirement — a box with plain `nightly` and
# no dated toolchain still builds, with a warning, rather than failing on a
# toolchain nobody asked it to install.
CLOCKWORK_RUST_NIGHTLY="${CLOCKWORK_RUST_NIGHTLY:-nightly-2026-07-02}"

# A toolchain that RESOLVES is not the same as one that WORKS. `rustup which`
# will happily auto-install a dated toolchain and hand back a path, and the
# binaries are then perfectly executable — but -Z build-std needs the std
# SOURCES, a separate component, and without them the build dies several
# minutes later on a missing library/Cargo.lock. So probe for rust-src too,
# which is what actually decides whether this toolchain can do the job.
resolve_nightly() {
    local tc="$1"
    command -v rustup >/dev/null 2>&1 || return 1
    CARGO_NIGHTLY="$(rustup which --toolchain "$tc" cargo 2>/dev/null)" || return 1
    RUSTC_NIGHTLY="$(rustup which --toolchain "$tc" rustc 2>/dev/null)" || return 1
    [ -x "$CARGO_NIGHTLY" ] && [ -x "$RUSTC_NIGHTLY" ] || return 1
    [ -f "${RUSTC_NIGHTLY%/bin/rustc}/lib/rustlib/src/rust/library/Cargo.lock" ]
}

if resolve_nightly "$CLOCKWORK_RUST_NIGHTLY"; then
    CARGO_NIGHTLY_ARGS=()
    echo "rust: $CLOCKWORK_RUST_NIGHTLY (pinned)"
elif resolve_nightly nightly; then
    CARGO_NIGHTLY_ARGS=()
    echo "rust: WARNING — $CLOCKWORK_RUST_NIGHTLY not installed, falling back to floating 'nightly'." >&2
    echo "      Install the pin with: rustup toolchain install $CLOCKWORK_RUST_NIGHTLY" >&2
elif cargo +nightly --version >/dev/null 2>&1; then
    CARGO_NIGHTLY="cargo"
    CARGO_NIGHTLY_ARGS=(+nightly)
    RUSTC_NIGHTLY=""      # the proxy resolves rustc itself
    echo "rust: floating 'nightly' via the cargo proxy" >&2
else
    echo "a nightly Rust toolchain is required (-Z build-std: the wasm heap is" >&2
    echo "shared with JS, so std must be rebuilt with +atomics,+bulk-memory)." >&2
    echo "  rustup toolchain install nightly" >&2
    echo "  rustup component add rust-src --toolchain nightly" >&2
    exit 1
fi

echo "building the Rust subsystems for wasm32-unknown-emscripten..."
# Exported inside the subshell, not written as an assignment prefix: a prefix
# only counts as one when it is literal at parse time, so `${VAR:+RUSTC=...}`
# after a line continuation expands into an ordinary word and bash tries to
# EXECUTE it ("No such file or directory: .../bin/rustc").
(
  cd "$ROOT/rust"
  export RUSTFLAGS="-C target-feature=+atomics,+bulk-memory,+mutable-globals"
  if [ -n "$RUSTC_NIGHTLY" ]; then export RUSTC="$RUSTC_NIGHTLY"; fi
  "$CARGO_NIGHTLY" "${CARGO_NIGHTLY_ARGS[@]}" build --release -p supersonic-native \
      --target wasm32-unknown-emscripten \
      --no-default-features $FEATURE_ARG \
      -Z build-std=std,panic_abort
)
RUST_LIB="$ROOT/rust/target/wasm32-unknown-emscripten/release/libsupersonic_native.a"
[ -f "$RUST_LIB" ] || { echo "Rust staticlib missing: $RUST_LIB" >&2; exit 1; }

# ── C++ ──────────────────────────────────────────────────────────────────────
SOURCES=(
    "$CLOCKWORK/src/audio_processor.cpp"
    # The client boundary (clockwork/src/clockwork_client.h). Every JS context
    # that touches a ring runs this rather than its own copy of the arithmetic.
    "$CLOCKWORK/src/clockwork_client.cpp"
    "$CLOCKWORK/src/lanes/lanes.cpp"
    "$CLOCKWORK/src/clock/ClockworkClock.cpp"
    "$CLOCKWORK/src/clock/EngineClock.cpp"
    "$CLOCKWORK/src/scope_streams.cpp"
    "$CLOCKWORK/src/engine_support.cpp"
    "$CLOCKWORK/src/clockwork_heap.cpp"
    "$CLOCKWORK/src/mem_region.cpp"
    "$CLOCKWORK/src/clock/ClockworkClockNative.cpp"
    "$CLOCKWORK/src/clock/TimeSource.cpp"
    "$CLOCKWORK/src/clock/MidiTimelines.cpp"
    "$CLOCKWORK/src/vendor/oscpack/osc/OscTypes.cpp"
    "$CLOCKWORK/src/vendor/oscpack/osc/OscOutboundPacketStream.cpp"
    "$CLOCKWORK/src/vendor/oscpack/osc/OscReceivedElements.cpp"
    "$CLOCKWORK/src/clock/MidiClockOut.cpp"
    # Audio files (clockwork/src/clockwork_audio_file.h): the same formats on
    # every platform, including the AIFF a browser's own decoder refuses.
    "$GUEST/scsynth_dsp.cpp"
    "$GUEST/piano_wavetable.cpp"
    # NOT SampleLoader.cpp: it needs JUCE, and there is no such host on the web
    # anyway — samples arrive already decoded through /b_allocPtr, which is
    # why supersonic-ugen-host declares its extern only for non-wasm targets.
    "$GUEST/node_tree.cpp"
    $SCSYNTH_SOURCES
    $SCSYNTH_C_SOURCES
)

# The audio file codecs (dr_wav, dr_flac, dr_mp3, stb_vorbis, the FLAC
# encoder) are a native concern: the sample loader and the recorder. On the
# web, samples arrive already decoded through the browser's own decoder and
# nothing records to a file, so nothing calls them — they were 200 kB of
# wasm nobody reached. Opt in with CLOCKWORK_WEB_AUDIO_FILES=1 for a build
# whose client decodes in the module.
AUDIO_FILE_EXPORTS=""
if [ "${CLOCKWORK_WEB_AUDIO_FILES:-0}" = "1" ]; then
    SOURCES+=("$CLOCKWORK/src/clockwork_audio_file.cpp" "$CLOCKWORK/src/flac_encoder.cpp" "$CLOCKWORK/src/vendor/stb/stb_vorbis.c")
    AUDIO_FILE_EXPORTS=",'_clockwork_audio_probe_memory','_clockwork_audio_decode_memory',\
'_clockwork_audio_free','_clockwork_audio_duration',\
'_clockwork_audio_can_write','_clockwork_audio_writer_open_memory',\
'_clockwork_audio_writer_write','_clockwork_audio_writer_frames',\
'_clockwork_audio_writer_close'"
fi

INCLUDES=(-I"$ROOT" -I"$CLOCKWORK" -I"$CLOCKWORK/src" -I"$CLOCKWORK/src/vendor/oscpack"
          -I"$GUEST" -I"$GUEST/synth/include/common" -I"$GUEST/synth/include/plugin_interface"
          -I"$GUEST/synth/include/server" -I"$GUEST/synth/common" -I"$GUEST/synth/server"
          -I"$GUEST/synth/external_libraries" -I"$GUEST/synth/external_libraries/nova-simd"
          -I"$GUEST/synth/external_libraries/TLSF-2.4.6/src")
for d in "$CLOCKWORK"/rust/*/cpp; do [ -d "$d" ] && INCLUDES+=(-I"$d"); done

# Every worklet entry point. A name missing here is dead-stripped and the
# worklet fails at run time, not at build time — so this list has to be right.
# It is clockwork's list, verbatim (clockwork/scripts/build-web.sh): the
# worklet is clockwork's, so the exports it needs are clockwork's to say.
EXPORTS="['___wasm_call_ctors','_clockwork_init','_get_ring_buffer_base',\
'_clockwork_tick','_process_audio','_get_audio_output_bus','_get_audio_input_bus',\
'_get_audio_num_output_buses','_get_audio_num_input_buses','_get_audio_buffer_samples',\
'_clockwork_clock_wasm_init','_set_time_offset','_get_time_offset',\
'_clockwork_log','_clockwork_log_va','_clockwork_log_raw',\
'_get_process_count','_get_messages_processed','_get_messages_dropped','_get_status_flags',\
'_clear_scheduler','_clockwork_host_forward','_clockwork_host_forwards','_malloc','_free',\
'_clockwork_client_abi_version','_clockwork_client_status_text',\
'_clockwork_client_open_memory','_clockwork_client_open_memory_in',\
'_clockwork_client_sizeof','_clockwork_client_close','_clockwork_client_info',\
'_clockwork_client_send','_clockwork_client_send_begin',\
'_clockwork_client_send_commit','_clockwork_client_send_abort',\
'_clockwork_client_poll','_clockwork_client_region',\
'_clockwork_client_metrics','_clockwork_client_clock','_clockwork_client_beat_at',\
'_clockwork_client_tap_sizeof','_clockwork_client_tap_open',\
'_clockwork_client_tap_open_in','_clockwork_client_tap_close',\
'_clockwork_client_tap_poll','_clockwork_client_tap_missed',\
'_clockwork_client_scope_open','_clockwork_client_scope_valid',\
'_clockwork_client_scope_audible_end','_clockwork_client_scope_read'"$AUDIO_FILE_EXPORTS"]"

# The engine's own web defines. NO_LIBSNDFILE is the one that matters: there is
# no libsndfile in a browser, and SC_SndFileHelpers.hpp has a whole alternative
# implementation behind that guard. (These live in the emcc line below; a
# comment cannot go inside it, because bash joins continued lines and would
# comment out every flag that followed.)
echo "compiling and linking..."
emcc "${SOURCES[@]}" "${INCLUDES[@]}" "$RUST_LIB" \
    -DNO_LIBSNDFILE \
    -DSTATIC_PLUGINS=1 \
    -DNOVA_SIMD \
    -DSC_FFT_GREEN \
    -DSC_AUDIO_API=3 \
    -DCLOCKWORK_GUEST=1 -DCLOCKWORK_SYNTH=1 -DCLOCKWORK_WORKLET_CLOCK=1 -DNDEBUG \
    -DCLOCKWORK_PRODUCT_NAME='"SuperSonic"' \
    -DCLOCKWORK_PRODUCT_VERSION="\"$PRODUCT_VERSION\"" \
    -DCLOCKWORK_PRODUCT_HEADER="\"$ROOT/dsp/supersonic_product.h\"" \
    -DCLOCKWORK_AUDIO_NO_STDIO=1 -DSTB_VORBIS_NO_STDIO \
    -DCLOCKWORK_SCHEDULER=$SCHEDULER \
    -DSCHEDULER_DATA_POOL_SIZE=$SCHEDULER_DATA_POOL_SIZE \
    -DSCHEDULER_SLOT_COUNT=$SCHEDULER_SLOT_COUNT \
    -DSCHEDULER_SHED_LATE_MS=0 \
    -DCLOCKWORK_IN_BUFFER_SIZE=$CLOCKWORK_IN_BUFFER_SIZE \
    -DCLOCKWORK_MEM_ARENA_SIZE=${MEM_ARENA_SIZE}u \
    -o "$OUT/scsynth-nrt.wasm" \
    -sSTANDALONE_WASM \
    -sNO_FILESYSTEM=1 \
    -sENVIRONMENT=worker \
    -pthread \
    -sALLOW_MEMORY_GROWTH=1 \
    -sINITIAL_MEMORY=$INITIAL_MEMORY \
    -sMAXIMUM_MEMORY=$MAX_MEMORY \
    -sSTACK_SIZE=$WASM_STACK_SIZE \
    -sEXPORTED_FUNCTIONS="$EXPORTS" \
    --no-entry \
    -Wl,--import-memory,--shared-memory,--allow-multiple-definition \
    -fcommon $OPT -msimd128 -flto -fwasm-exceptions \
    -fno-math-errno -fsigned-zeros -fno-associative-math \
    -Wno-nontrivial-memcall -Wno-extern-c-compat \
    -sASSERTIONS=${CLOCKWORK_ASSERTIONS:-0} \
    -sSAFE_HEAP=${CLOCKWORK_SAFE_HEAP:-0} \
    ${CLOCKWORK_KEEP_NAMES:+--profiling-funcs} \
    -sERROR_ON_UNDEFINED_SYMBOLS=1

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
