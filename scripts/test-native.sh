#!/bin/bash

# Build and run the native (Catch2) test suite
#
# Usage:
#   scripts/test-native.sh                          # build + run tests (headless)
#   scripts/test-native.sh --debug                  # build + run tests (Debug)
#   scripts/test-native.sh --clean                  # clean rebuild + run tests
#   scripts/test-native.sh --device "MacBook Pro Speakers" # test against a real device

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
            # The name test_scheduling_accuracy.cpp reads: without it a
            # --device run is quietly a headless one.
            export CLOCKWORK_TEST_DEVICE="$2"
            shift 2
            ;;
        --help|-h)
            echo "Usage: $0 [options]"
            echo "  --debug         Build in Debug mode (default: Release)"
            echo "  --clean         Remove build dir and reconfigure"
            echo "  --device NAME   Test against a real audio device, by name; one that matches none opens the default"
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

# Run tests the two ways CI does (.github/workflows/native.yml says why):
# through ctest, every Catch2 case in a process of its own (benchmarks
# excluded by the discovery) with the guest boundary check; then the binary,
# every case in one process, in a shuffled order.
echo ""
echo "Running native tests, each case on its own..."
ctest --test-dir "$BUILD_DIR" -C "$BUILD_TYPE" --output-on-failure -j 4 --timeout 300

TEST_BINARY="$BUILD_DIR/test/native/SuperSonicNativeTests"
if [ ! -f "$TEST_BINARY" ] && [ ! -f "$TEST_BINARY.exe" ]; then
    TEST_BINARY="$BUILD_DIR/test/native/$BUILD_TYPE/SuperSonicNativeTests"
fi
echo ""
echo "Running native tests, one process, shuffled..."
"$TEST_BINARY" "~[benchmark]" --order rand
