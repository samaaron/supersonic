#!/bin/bash

# Run the WASM/Playwright test suite
# Requires: build-web.sh to have been run first, npm install, playwright installed
#
# Usage:
#   scripts/test-web.sh              # full suite (summary only)
#   scripts/test-web.sh --verbose    # full suite with failure details
#   scripts/test-web.sh FILE         # single test file
#   scripts/test-web.sh --project=SAB  # single project (SAB or postMessage)

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
cd "$PROJECT_ROOT"

# Check prerequisites
if [ ! -f dist/supersonic.js ]; then
    echo "Error: dist/supersonic.js not found. Run scripts/build-web.sh first."
    exit 1
fi

TAIL_LINES=10
ARGS=()

for arg in "$@"; do
    case $arg in
        --verbose|-v) TAIL_LINES=30 ;;
        --help|-h)
            echo "Usage: $0 [options] [playwright args...]"
            echo "  --verbose, -v   Show more output (last 30 lines instead of 10)"
            echo "  FILE            Run a single test file"
            echo "  --project=NAME  Run a single project (SAB or postMessage)"
            echo ""
            echo "Examples:"
            echo "  $0                              # full suite, summary only"
            echo "  $0 --verbose                    # full suite, show failures"
            echo "  $0 test/osc_semantic.spec.mjs   # single file"
            echo "  $0 --project=SAB                # SAB tests only"
            exit 0
            ;;
        *) ARGS+=("$arg") ;;
    esac
done

echo "Running WASM tests..."
npx playwright test --reporter=line "${ARGS[@]}" 2>&1 | tail -"$TAIL_LINES"
