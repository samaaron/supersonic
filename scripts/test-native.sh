#!/bin/bash

# Build and run the native (Catch2) test suite
#
# Usage:
#   scripts/test-native.sh            # build + run tests (Release)
#   scripts/test-native.sh --debug    # build + run tests (Debug)
#   scripts/test-native.sh --clean    # clean rebuild + run tests

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="$PROJECT_ROOT/build/native"

BUILD_TYPE="Release"
CLEAN=false

for arg in "$@"; do
    case $arg in
        --debug)    BUILD_TYPE="Debug" ;;
        --clean)    CLEAN=true ;;
        --help|-h)
            echo "Usage: $0 [options]"
            echo "  --debug    Build in Debug mode (default: Release)"
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
    echo "Cleaning build directory..."
    rm -rf "$BUILD_DIR"
fi

echo "Building native tests ($BUILD_TYPE)..."
cmake -B "$BUILD_DIR" \
    -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
    -DBUILD_TESTS=ON \
    "$PROJECT_ROOT"

cmake --build "$BUILD_DIR" --config "$BUILD_TYPE" --parallel

# Run tests
TEST_BINARY="$BUILD_DIR/test/native/SuperSonicNativeTests"
if [ -f "$TEST_BINARY" ]; then
    echo ""
    echo "Running native tests..."
    "$TEST_BINARY"
else
    echo "Error: Test binary not found at $TEST_BINARY"
    exit 1
fi
