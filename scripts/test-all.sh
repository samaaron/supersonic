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

echo "=== All test suites passed ==="
