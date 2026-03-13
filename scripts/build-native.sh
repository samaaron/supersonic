#!/bin/bash

# Build script for SuperSonic native (JUCE) backend
# Uses CMake to build the SuperSonic executable

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="$PROJECT_ROOT/build/native"

# Parse arguments
BUILD_TYPE="Release"
BUILD_TESTS=OFF
CLEAN=false
JOBS=""

for arg in "$@"; do
    case $arg in
        --debug)    BUILD_TYPE="Debug" ;;
        --tests)    BUILD_TESTS=ON ;;
        --clean)    CLEAN=true ;;
        --jobs=*)   JOBS="${arg#*=}" ;;
        --help|-h)
            echo "Usage: $0 [options]"
            echo "  --debug     Build in Debug mode (default: Release)"
            echo "  --tests     Build native test suite"
            echo "  --clean     Remove build dir and reconfigure"
            echo "  --jobs=N    Parallel build jobs (default: auto)"
            exit 0
            ;;
        *)
            echo "Unknown option: $arg"
            exit 1
            ;;
    esac
done

echo "Building SuperSonic native ($BUILD_TYPE)"

if [ "$CLEAN" = true ] && [ -d "$BUILD_DIR" ]; then
    echo "Cleaning build directory..."
    rm -rf "$BUILD_DIR"
fi

# Configure (always pass options so flags like --tests aren't silently ignored)
if [ ! -f "$BUILD_DIR/CMakeCache.txt" ]; then
    echo "Configuring CMake..."
else
    echo "Reconfiguring CMake..."
fi
cmake -B "$BUILD_DIR" \
    -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
    -DBUILD_TESTS="$BUILD_TESTS" \
    "$PROJECT_ROOT"

# Build
BUILD_ARGS=(--build "$BUILD_DIR" --config "$BUILD_TYPE")
if [ -n "$JOBS" ]; then
    BUILD_ARGS+=(--parallel "$JOBS")
fi

cmake "${BUILD_ARGS[@]}"

# Report
BINARY="$BUILD_DIR/SuperSonic_artefacts/$BUILD_TYPE/SuperSonic"
if [ -f "$BINARY" ]; then
    echo ""
    echo "Built: $BINARY"
    ls -lh "$BINARY"
else
    echo ""
    echo "Build complete. Check $BUILD_DIR for output."
fi
