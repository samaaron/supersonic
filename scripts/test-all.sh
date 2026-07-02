#!/bin/bash

# Run ALL test suites: native, NIF, and WASM/Playwright
# This mirrors what CI does across the three workflows.
#
# Usage:
#   scripts/test-all.sh            # run everything
#   scripts/test-all.sh --clean    # clean rebuild + run everything

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

CLEAN_FLAG=""
for arg in "$@"; do
    case $arg in
        --clean) CLEAN_FLAG="--clean" ;;
        --help|-h)
            echo "Usage: $0 [--clean]"
            echo "  Runs all test suites: native, NIF, and WASM/Playwright"
            echo "  --clean    Clean rebuild before each suite"
            exit 0
            ;;
    esac
done

echo "=== Native tests ==="
"$SCRIPT_DIR/test-native.sh" $CLEAN_FLAG
echo ""

echo "=== NIF tests ==="
"$SCRIPT_DIR/test-nif.sh" $CLEAN_FLAG
echo ""

echo "=== WASM tests ==="
"$SCRIPT_DIR/test-web.sh"
echo ""

echo "=== No-synth runtime smoke (MIT core) ==="
# Mirrors CI's scheduler-host-mit job: the only runtime exercise of the
# no-synth engine core (boot, tick, scheduler fire).
ROOT_DIR="$(dirname "$SCRIPT_DIR")"
NOSYNTH_BUILD="$ROOT_DIR/build/nosynth-smoke"
if [ -n "$CLEAN_FLAG" ]; then rm -rf "$NOSYNTH_BUILD"; fi
cmake -B "$NOSYNTH_BUILD" -DCMAKE_BUILD_TYPE=Release \
    -DSUPERSONIC_ENABLE_SYNTH=OFF -DSUPERSONIC_ENABLE_LINK=OFF \
    > /dev/null
cmake --build "$NOSYNTH_BUILD" --parallel --target supersonic_nosynth_smoke > /dev/null
"$NOSYNTH_BUILD/supersonic_nosynth_smoke"
echo ""

echo "=== All test suites passed ==="
