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
#   scripts/export-site.sh [OUT]     (default: build/site)
#
# Upload OUT as the site's supersonic/ directory. The host must send
# Cross-Origin-Opener-Policy: same-origin and
# Cross-Origin-Embedder-Policy: require-corp, or the browser withholds
# SharedArrayBuffer and the SAB transport cannot start.

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="${1:-$ROOT/build/site}"

if [ ! -f "$ROOT/dist/supersonic.js" ] || [ ! -d "$ROOT/dist/wasm" ]; then
  echo "export-site: no build in $ROOT/dist — run scripts/build-web.sh first" >&2
  exit 1
fi

rm -rf "$OUT"
mkdir -p "$OUT"
cp -R "$ROOT/example/." "$OUT/"
rm -f "$OUT/dist"
cp -R "$ROOT/dist" "$OUT/dist"

echo "exported to $OUT ($(du -sh "$OUT" | cut -f1))"
