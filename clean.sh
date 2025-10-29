#!/bin/bash

# Clean script for Supersonic
# Removes build artifacts and old versioned WASM files

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$SCRIPT_DIR"
DIST_DIR="$PROJECT_ROOT/dist"
SRC_DIR="$PROJECT_ROOT/src"

echo "Cleaning Supersonic build artifacts..."

# Remove auto-generated build info header
if [ -f "$SRC_DIR/build_info.h" ]; then
    echo "  Removing build_info.h"
    rm "$SRC_DIR/build_info.h"
fi

# Clean dist directory
if [ -d "$DIST_DIR" ]; then
    echo "  Removing dist/"
    rm -rf "$DIST_DIR"
fi

echo "Clean complete!"
echo ""
echo "To rebuild, run: ./build.sh"
