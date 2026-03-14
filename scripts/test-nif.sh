#!/bin/bash

# Build the NIF shared library and run the Elixir test suite
#
# Requires: cmake, Erlang/OTP 27+, Elixir 1.18+
#
# Usage:
#   scripts/test-nif.sh            # build NIF + run Elixir tests
#   scripts/test-nif.sh --clean    # clean rebuild + run tests

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="$PROJECT_ROOT/build/nif"

CLEAN=false

for arg in "$@"; do
    case $arg in
        --clean)    CLEAN=true ;;
        --help|-h)
            echo "Usage: $0 [options]"
            echo "  --clean    Remove build dir and reconfigure"
            exit 0
            ;;
        *)
            echo "Unknown option: $arg"
            exit 1
            ;;
    esac
done

if [ "$CLEAN" = true ] && [ -d "$BUILD_DIR" ]; then
    echo "Cleaning NIF build directory..."
    rm -rf "$BUILD_DIR"
fi

echo "Building NIF shared library..."
cmake -B "$BUILD_DIR" \
    -DBUILD_NIF=ON \
    -DCMAKE_BUILD_TYPE=Release \
    "$PROJECT_ROOT"

cmake --build "$BUILD_DIR" --target supersonic_nif --config Release --parallel

# Verify NIF binary exists
NIF_EXT="so"
if [[ "$OSTYPE" == "msys" || "$OSTYPE" == "cygwin" || "$OSTYPE" == "win32" ]]; then
    NIF_EXT="dll"
fi

NIF_PATH="$BUILD_DIR/supersonic.$NIF_EXT"
if [ ! -f "$NIF_PATH" ]; then
    echo "Error: NIF binary not found at $NIF_PATH"
    exit 1
fi
echo "NIF binary: $NIF_PATH"

# Run Elixir tests
echo ""
echo "Running Elixir tests..."
cd "$PROJECT_ROOT/test/nif"

# Install deps if needed
if [ ! -d "_build" ]; then
    mix local.hex --force --if-missing
    mix local.rebar --force --if-missing
fi

SUPERSONIC_NIF_PATH="$BUILD_DIR" SUPERSONIC_HEADLESS=1 mix test
