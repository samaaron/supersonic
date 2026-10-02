#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2025-2026 Sam Aaron
#
# export-site.sh — assemble the demo as it is published (sonic-pi.net/supersonic).
#
# The example pages refer to "dist/..." and "assets/...", so a published copy
# is the example directory with the built library sitting inside it as a real
# directory. In the repository that "dist" is a symlink to ../dist, which a
# web host does not follow; here it becomes a copy.
#
#   scripts/export-site.sh [--cdn] [OUT]     (default OUT: build/site)
#
# --cdn exports what sonic-pi.net serves: demo.html with its assets and the
# library, but not the samples and synthdefs (most of dist's size) — the demo
# fetches those from the npm packages on jsDelivr instead, pinned to the
# packages' versions here, which must therefore be published. jsDelivr sends
# CORS and Cross-Origin-Resource-Policy: cross-origin, so they load under the
# page's COEP. The library, wasm and workers stay on the host: the SAB
# transport needs them same-origin. Only the demo is configured for the CDN,
# so the other example pages are left out.
#
# Without --cdn everything is local, which is what the test suite boots
# (test/global-setup.mjs) — it needs no network and no published release.
#
# Upload OUT as the site's supersonic/ directory. The host must send
# Cross-Origin-Opener-Policy: same-origin and
# Cross-Origin-Embedder-Policy: require-corp, or the browser withholds
# SharedArrayBuffer and the SAB transport cannot start.

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CDN_MODE=0
if [ "${1:-}" = "--cdn" ]; then CDN_MODE=1; shift; fi
OUT="${1:-$ROOT/build/site}"

if [ ! -f "$ROOT/dist/supersonic.js" ] || [ ! -d "$ROOT/dist/wasm" ]; then
  echo "export-site: no build in $ROOT/dist — run scripts/build-web.sh first" >&2
  exit 1
fi

rm -rf "$OUT"
mkdir -p "$OUT"

if [ "$CDN_MODE" = 0 ]; then
  cp -R "$ROOT/example/." "$OUT/"
  rm -f "$OUT/dist"
  cp -R "$ROOT/dist" "$OUT/dist"
  echo "exported to $OUT ($(du -sh "$OUT" | cut -f1))"
  exit 0
fi

CDN="https://cdn.jsdelivr.net/npm"
version() { node -p "require('$ROOT/packages/$1/package.json').version"; }
SAMPLES="$CDN/supersonic-scsynth-samples@$(version supersonic-scsynth-samples)/samples/"
SYNTHDEFS="$CDN/supersonic-scsynth-synthdefs@$(version supersonic-scsynth-synthdefs)/synthdefs/"

mkdir -p "$OUT/dist"
cp "$ROOT/example/demo.html" "$ROOT/example/favicon.png" "$OUT/"
cp -R "$ROOT/example/assets" "$OUT/assets"
for f in "$ROOT/dist/"*; do
  case "$(basename "$f")" in samples|synthdefs) continue ;; esac
  cp -R "$f" "$OUT/dist/"
done

# Point the demo's SuperSonic at the CDN for samples and synthdefs.
APP="$OUT/assets/app.js"
LINE='      baseURL: "dist/",'
if [ "$(grep -cxF "$LINE" "$APP")" != 1 ]; then
  echo "export-site: expected one '$LINE' in example/assets/app.js" >&2
  exit 1
fi
awk -v line="$LINE" -v samples="$SAMPLES" -v synthdefs="$SYNTHDEFS" '
  { print }
  $0 == line {
    print "      sampleBaseURL: \"" samples "\","
    print "      synthdefBaseURL: \"" synthdefs "\","
  }' "$APP" > "$APP.tmp" && mv "$APP.tmp" "$APP"

echo "samples   → $SAMPLES"
echo "synthdefs → $SYNTHDEFS"
echo "exported to $OUT ($(du -sh "$OUT" | cut -f1))"
