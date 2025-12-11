#!/bin/bash
set -e

# SuperSonic Demo Packaging Script
#
# Creates a standalone zip of the demo that can be deployed to any web server
# Includes all necessary files: HTML, JS, WASM, samples, and synthdefs

# Color output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;36m'
NC='\033[0m' # No Color

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
VERSION=$(node -p "require('./package.json').version")
OUTPUT_NAME="supersonic-demo-v${VERSION}"
TEMP_DIR="${PROJECT_ROOT}/tmp/${OUTPUT_NAME}"
OUTPUT_FILE="${PROJECT_ROOT}/${OUTPUT_NAME}.zip"

echo "========================================"
echo "  SuperSonic Demo Packaging"
echo "========================================"
echo ""
echo -e "${YELLOW}Version: ${VERSION}${NC}"
echo -e "${YELLOW}Output: ${OUTPUT_NAME}.zip${NC}"
echo ""

# Clean up any previous temp directory
if [ -d "$TEMP_DIR" ]; then
    echo "Cleaning up previous temp directory..."
    rm -rf "$TEMP_DIR"
fi

# Create temp directory structure
echo "Creating package structure..."
mkdir -p "$TEMP_DIR"
mkdir -p "$TEMP_DIR/assets"
mkdir -p "$TEMP_DIR/dist/wasm"
mkdir -p "$TEMP_DIR/dist/workers"
mkdir -p "$TEMP_DIR/dist/samples"
mkdir -p "$TEMP_DIR/dist/synthdefs"

# Copy demo HTML and favicon
echo "Copying demo files..."
cp "$PROJECT_ROOT/example/demo.html" "$TEMP_DIR/"
cp "$PROJECT_ROOT/example/favicon.png" "$TEMP_DIR/"

# Copy demo assets (only files needed by demo.html)
echo "Copying demo assets..."
cp "$PROJECT_ROOT/example/assets/app.js" "$TEMP_DIR/assets/"
cp "$PROJECT_ROOT/example/assets/node_tree_viz.js" "$TEMP_DIR/assets/"
cp "$PROJECT_ROOT/example/assets/"*.png "$TEMP_DIR/assets/"
# Ensure all assets are world-readable
chmod 644 "$TEMP_DIR/assets/"*

# Set production mode for packaged demo (disables manifest loading, debug logging)
sed -i 's|/\* DEMO_BUILD_CONFIG \*/ await orchestrator.init({ development: true });|await orchestrator.init({ development: false });|' "$TEMP_DIR/assets/app.js"

# Copy dist files
echo "Copying distribution files..."
cp "$PROJECT_ROOT/dist/supersonic.js" "$TEMP_DIR/dist/"

# Copy WASM files (following symlinks)
# Note: We only copy the stable (non-hashed) WASM file, not the manifest
# This ensures the demo uses the stable filename instead of trying to load a hashed version
echo "Copying WASM files..."
cp -L "$PROJECT_ROOT/dist/wasm/scsynth-nrt.wasm" "$TEMP_DIR/dist/wasm/"

# Copy workers
echo "Copying worker files..."
cp "$PROJECT_ROOT/dist/workers/"*.js "$TEMP_DIR/dist/workers/"

# Copy samples (following symlinks)
echo "Copying samples (206 files)..."
cp -L "$PROJECT_ROOT/packages/supersonic-scsynth-samples/samples/"* "$TEMP_DIR/dist/samples/"

# Copy synthdefs (following symlinks)
echo "Copying synthdefs (120 files)..."
cp -L "$PROJECT_ROOT/packages/supersonic-scsynth-synthdefs/synthdefs/"* "$TEMP_DIR/dist/synthdefs/"

# Create the zip file
echo ""
echo "Creating zip archive..."
cd "$PROJECT_ROOT/tmp"
zip -r "$OUTPUT_NAME.zip" "$OUTPUT_NAME" > /dev/null

# Move zip to project root (overwrite if exists)
mv -f "$OUTPUT_NAME.zip" "$PROJECT_ROOT/"

# Calculate size
SIZE=$(du -h "$OUTPUT_FILE" | cut -f1)

# Clean up temp directory
echo "Cleaning up..."
rm -rf "$TEMP_DIR"

echo ""
echo -e "${GREEN}========================================"
echo "Package created successfully! 🎉"
echo "========================================${NC}"
echo ""
echo "File: ${OUTPUT_NAME}.zip"
echo "Size: ${SIZE}"
echo ""
echo "Contents:"
echo "  ✓ Demo HTML and assets"
echo "  ✓ SuperSonic v${VERSION} library"
echo "  ✓ WebAssembly binary (1.4 MB)"
echo "  ✓ 206 audio samples"
echo "  ✓ 120 synth definitions"
echo ""
echo "Ready to deploy to your web server!"
echo ""
