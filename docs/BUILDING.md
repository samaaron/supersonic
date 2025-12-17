# Building from Source

If you want to compile SuperSonic yourself, here's how.

## Prerequisites

**Emscripten SDK**

You'll need the Emscripten compiler to build the WASM files:

```bash
# Clone emsdk
git clone https://github.com/emscripten-core/emsdk.git
cd emsdk

# Install and activate latest version
./emsdk install latest
./emsdk activate latest

# Add to your shell (do this before building)
source ./emsdk_env.sh
```

See the [Emscripten documentation](https://emscripten.org/docs/getting_started/downloads.html) for more details.

**Node.js**

Required for esbuild and the JavaScript bundling:

```bash
npm install
```

## Building

Make sure Emscripten is activated in your current shell:

```bash
source ~/path/to/emsdk_env.sh
```

Then run the build script:

```bash
./build.sh
```

This will:
1. Compile the SuperCollider engine to WebAssembly
2. Bundle the JavaScript files with esbuild
3. Copy workers, WASM, synthdefs, and samples to `dist/`

## Output

After building, you'll find everything in the `dist/` directory:

```
dist/
├── supersonic.js                 # Main entry point (ES module)
├── wasm/
│   └── scsynth-nrt.wasm          # Audio engine (~1.5MB)
├── workers/
│   ├── scsynth_audio_worklet.js  # AudioWorklet processor
│   ├── osc_in_worker.js          # OSC input handler
│   ├── osc_out_prescheduler_worker.js  # OSC pre-scheduler
│   └── debug_worker.js           # Debug logger
├── synthdefs/                    # Compiled synth definitions
└── samples/                      # Audio samples
```

## Running the Demo

Start the test server:

```bash
cd example && npx serve
```

Open http://localhost:3000/demo.html

## Docker

If you prefer not to install Emscripten locally, you can use Docker:

```bash
# Build the image
docker build -t supersonic .

# Run the container (serves on port 8002)
docker run --rm -it -p 8002:8002 supersonic
```

Open http://localhost:8002/demo.html

## Development Tips

**Rebuild just JavaScript:**

If you're only changing JavaScript files, you can skip the WASM compilation:

```bash
npx esbuild js/supersonic.js --bundle --format=esm --outfile=dist/supersonic.js
```

**Watch mode:**

For rapid iteration on JavaScript:

```bash
npx esbuild js/supersonic.js --bundle --format=esm --outfile=dist/supersonic.js --watch
```

## Troubleshooting

### Emscripten not found

Make sure you've run `source emsdk_env.sh` in your current shell session.

### WASM compilation fails

Check that you have enough memory. The compilation can use several GB of RAM.

### Build takes a long time

The first build compiles the entire SuperCollider engine, which takes a while. Subsequent builds are faster as unchanged files are cached.
