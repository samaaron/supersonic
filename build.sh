#!/bin/bash

# Build script for Supersonic
# Compiles C++ to WebAssembly using Emscripten

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$SCRIPT_DIR"
SRC_DIR="$PROJECT_ROOT/src"
JS_DIR="$PROJECT_ROOT/js"
OUTPUT_DIR="$PROJECT_ROOT/dist"

echo "Building Supersonic..."
echo "Source: $SRC_DIR"
echo "Output: $OUTPUT_DIR"

# Create output directories if they don't exist
mkdir -p "$OUTPUT_DIR/wasm"

# Check if emcc is available
if ! command -v emcc &> /dev/null; then
    echo "Error: emcc (Emscripten compiler) not found!"
    echo "Please install and activate the Emscripten SDK first:"
    echo "  source ~/Development/sc/emsdk/emsdk_env.sh"
    exit 1
fi

echo "Using Emscripten version:"
emcc --version | head -1

# Build standalone WASM with shared memory import and scsynth
echo "Compiling C++ to WASM with scsynth..."

# Collect all scsynth source files
# Exclude SC_ComPort.cpp (has nova-tt dependencies, replaced by SC_OscUnroll.cpp)
SCSYNTH_SERVER_SOURCES=$(find "$SRC_DIR/scsynth/server" -name "*.cpp" ! -name "SC_*Plugins.cpp" ! -name "scsynth_main.cpp" ! -name "SC_WebAudio.cpp" ! -name "SC_Wasm.cpp" ! -name "SC_WasmOscBuilder.cpp" ! -name "SC_ComPort.cpp" 2>/dev/null | tr '\n' ' ')
SCSYNTH_COMMON_SOURCES=$(find "$SRC_DIR/scsynth/common" -name "*.cpp" ! -name "XenomaiLock.cpp" ! -name "SC_PaUtils.cpp" ! -name "sc_popen.cpp" ! -name "strtod.c" 2>/dev/null | tr '\n' ' ')
SCSYNTH_PLUGIN_SOURCES=$(find "$SRC_DIR/scsynth/plugins" -name "*.cpp" 2>/dev/null | tr '\n' ' ')

# Compile audio processor with all scsynth sources and oscpack (standalone WASM for AudioWorklet)
emcc "$SRC_DIR/audio_processor.cpp" \
    "$SRC_DIR/scsynth/server/SC_OscUnroll.cpp" \
    $SCSYNTH_SERVER_SOURCES \
    $SCSYNTH_COMMON_SOURCES \
    $SCSYNTH_PLUGIN_SOURCES \
    "$SRC_DIR/vendor/oscpack/osc/OscTypes.cpp" \
    "$SRC_DIR/vendor/oscpack/osc/OscOutboundPacketStream.cpp" \
    "$SRC_DIR/vendor/oscpack/osc/OscReceivedElements.cpp" \
    -I"$SRC_DIR" \
    -I"$SRC_DIR/scsynth/include/common" \
    -I"$SRC_DIR/scsynth/include/plugin_interface" \
    -I"$SRC_DIR/scsynth/include/server" \
    -I"$SRC_DIR/scsynth/server" \
    -I"$SRC_DIR/scsynth/common" \
    -I"$SRC_DIR/scsynth/external_libraries/boost" \
    -I"$SRC_DIR/scsynth/external_libraries/nova-simd" \
    -I"$SRC_DIR/vendor/oscpack" \
    -DNO_LIBSNDFILE \
    -DNDEBUG \
    -DBOOST_ASIO_HAS_PTHREADS \
    -DSTATIC_PLUGINS \
    -DNOVA_SIMD \
    -sSTANDALONE_WASM \
    -sNO_FILESYSTEM=1 \
    -sENVIRONMENT=worker \
    -pthread \
    -sALLOW_MEMORY_GROWTH=0 \
    -sINITIAL_MEMORY=33554432 \
    -sEXPORTED_FUNCTIONS="['___wasm_call_ctors','_get_ring_buffer_base','_get_buffer_layout','_init_memory','_process_audio','_get_audio_output_bus','_get_audio_buffer_samples','_get_supersonic_version_string','_set_time_offset','_get_time_offset','_worklet_debug','_worklet_debug_va','_get_process_count','_get_messages_processed','_get_messages_dropped','_get_status_flags']" \
    --no-entry \
    -Wl,--import-memory,--shared-memory,--allow-multiple-definition \
    -fcommon \
    -O3 \
    -ffast-math \
    -msimd128 \
    -flto \
    -sERROR_ON_UNDEFINED_SYMBOLS=0 \
    -o "$OUTPUT_DIR/wasm/scsynth-nrt.wasm"

# Check if esbuild is available
if ! command -v esbuild &> /dev/null; then
    echo "Error: esbuild not found!"
    echo "Please install esbuild: npm install -g esbuild"
    exit 1
fi

echo "Bundling JavaScript with esbuild..."

# Bundle the main orchestrator (entry point for the library)
# osc.js is now a pure ES module (converted from UMD) and can be bundled
esbuild "$JS_DIR/supersonic.js" \
    --bundle \
    --format=esm \
    --outfile="$OUTPUT_DIR/supersonic.js" \
    --external:./scsynth-nrt.wasm

# Copy worker files to workers subdirectory (these need to remain separate as they run in different contexts)
echo "Copying worker files..."
mkdir -p "$OUTPUT_DIR/workers"
cp "$JS_DIR/workers/"* "$OUTPUT_DIR/workers/"

# Copy binary synthdefs
SYNTHDEFS_SRC="$PROJECT_ROOT/etc/synthdefs"
if [ -d "$SYNTHDEFS_SRC" ]; then
    echo "Copying binary synthdefs..."
    mkdir -p "$OUTPUT_DIR/etc/synthdefs"
    cp "$SYNTHDEFS_SRC/"*.scsyndef "$OUTPUT_DIR/etc/synthdefs/" 2>/dev/null || true

    # Generate manifest.json with list of available synthdefs
    echo "Generating synthdef manifest..."
    echo '{' > "$OUTPUT_DIR/etc/synthdefs/manifest.json"
    echo '  "synthdefs": [' >> "$OUTPUT_DIR/etc/synthdefs/manifest.json"
    ls "$OUTPUT_DIR/etc/synthdefs/"*.scsyndef 2>/dev/null | \
        sed 's|.*/||; s|\.scsyndef$||' | \
        awk '{printf "    \"%s\"%s\n", $0, (NR==1?",":",")}' | \
        sed '$ s/,$//' >> "$OUTPUT_DIR/etc/synthdefs/manifest.json" 2>/dev/null || echo '    "sonic-pi-beep"' >> "$OUTPUT_DIR/etc/synthdefs/manifest.json"
    echo '  ]' >> "$OUTPUT_DIR/etc/synthdefs/manifest.json"
    echo '}' >> "$OUTPUT_DIR/etc/synthdefs/manifest.json"

    echo "Copied $(ls -1 "$OUTPUT_DIR/etc/synthdefs/"*.scsyndef 2>/dev/null | wc -l) synthdef files"
else
    echo "Warning: Synthdefs not found at $SYNTHDEFS_SRC"
fi

echo "Build complete!"
echo "Generated files:"
ls -lh "$OUTPUT_DIR"
