#!/bin/bash

# Build and run the native (Catch2) test suite
#
# Usage:
#   scripts/test-native.sh                          # build + run tests (headless)
#   scripts/test-native.sh --debug                  # build + run tests (Debug)
#   scripts/test-native.sh --clean                  # clean rebuild + run tests
#   scripts/test-native.sh --device "Windows Audio" # test against real hardware

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="$PROJECT_ROOT/build/native"

BUILD_TYPE="Release"
CLEAN=false

while [[ $# -gt 0 ]]; do
    case $1 in
        --debug)    BUILD_TYPE="Debug"; shift ;;
        --clean)    CLEAN=true; shift ;;
        --device)
            export SUPERSONIC_TEST_DEVICE="$2"
            shift 2
            ;;
        --help|-h)
            echo "Usage: $0 [options]"
            echo "  --debug         Build in Debug mode (default: Release)"
            echo "  --clean         Remove build dir and reconfigure"
            echo "  --device NAME   Test against a real audio driver (e.g. \"Windows Audio\")"
            exit 0
            ;;
        *)
            echo "Unknown option: $1"
            exit 1
            ;;
    esac
done

if [ "$CLEAN" = true ] && [ -d "$BUILD_DIR" ]; then
    echo "Cleaning build directory..."
    rm -rf "$BUILD_DIR"
fi

echo "Building native tests ($BUILD_TYPE)..."
cmake -B "$BUILD_DIR" \
    -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
    -DBUILD_TESTS=ON \
    "$PROJECT_ROOT"

cmake --build "$BUILD_DIR" --config "$BUILD_TYPE" --parallel

# Run tests — check MSVC multi-config path as fallback (Windows)
TEST_BINARY="$BUILD_DIR/test/native/SuperSonicNativeTests"
if [ ! -f "$TEST_BINARY" ] && [ ! -f "$TEST_BINARY.exe" ]; then
    TEST_BINARY="$BUILD_DIR/test/native/$BUILD_TYPE/SuperSonicNativeTests"
fi

if [ -f "$TEST_BINARY" ] || [ -f "$TEST_BINARY.exe" ]; then
    echo ""
    echo "Running native tests..."
    "$TEST_BINARY"
else
    echo "Error: Test binary not found"
    exit 1
fi
