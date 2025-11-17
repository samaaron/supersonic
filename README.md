# SuperSonic

> **Warning - Super Alpha Status**: SuperSonic is currently in active development. The API is likely to change between releases. Feedback welcome!

A WebAssembly port of SuperCollider's scsynth audio synthesis engine for the browser. Runs in an AudioWorklet for real-time, high-priority audio processing with full OSC API support.

## Quick Start

```html
<script type="module">
  import { SuperSonic } from './dist/supersonic.js';

  const sonic = new SuperSonic({
    workerBaseURL: './dist/workers/',
    wasmBaseURL: './dist/wasm/',
    sampleBaseURL: './dist/samples/',
    synthdefBaseURL: './dist/synthdefs/'
  });

  await sonic.init();

  // Load a synthdef
  await sonic.loadSynthDefs(['sonic-pi-beep']);

  // Trigger the synth
  sonic.send('/s_new', 'sonic-pi-beep', -1, 0, 0, 'note', 60);

  // Load and play a sample
  sonic.send('/b_allocRead', 0, 'bd_haus.flac');
  sonic.send('/s_new', 'sonic-pi-basic_mono_player', -1, 0, 0, 'buf', 0);
</script>
```

**Important:** SuperSonic requires self-hosting (cannot load from CDN). See [CDN Usage](#cdn-usage) below.

## Installation

**Via npm (for local bundling):**
```bash
# Core engine only (~450KB)
npm install supersonic-scsynth

# Everything (engine + synthdefs + samples)
npm install supersonic-scsynth-bundle
```

**Pre-built distribution (recommended):**
Download the pre-built package (~35MB with all synthdefs and samples) and serve from your own domain:
https://samaaron.github.io/supersonic/supersonic-dist.zip

Extract to your web server and import as:
```javascript
import { SuperSonic } from './dist/supersonic.js';
```

## Packages

SuperSonic is split into multiple packages:

| Package | Size | License | Contents |
|---------|------|---------|----------|
| `supersonic-scsynth` | ~450KB | GPL-3.0-or-later | Core WASM engine |
| `supersonic-scsynth-synthdefs` | ~67KB | MIT | 120 Sonic Pi synthdefs |
| `supersonic-scsynth-samples` | ~34MB | CC0-1.0 | 206 Sonic Pi samples |
| `supersonic-scsynth-bundle` | - | - | All of the above |

All synthdefs and samples are from [Sonic Pi](https://github.com/sonic-pi-net/sonic-pi).

## API Reference

**Creating an instance:**
```javascript
const sonic = new SuperSonic({
  workerBaseURL: './dist/workers/',  // Required: Path to worker files
  wasmBaseURL: './dist/wasm/',       // Required: Path to WASM files
  sampleBaseURL: './dist/samples/',  // Optional: Path to audio samples
  synthdefBaseURL: './dist/synthdefs/', // Optional: Path to synthdefs
  audioPathMap: { /* optional custom path mappings */ }
});
```

**Core methods:**
- `await sonic.init()` - Initialize the audio engine
- `await sonic.loadSynthDefs(names)` - Load synth definitions
- `sonic.send(address, ...args)` - Send OSC message (types auto-detected)
- `sonic.sendOSC(oscBytes, options)` - Send pre-encoded OSC bytes

**Callbacks:**
- `sonic.onInitialized` - Called when ready
- `sonic.onError(error)` - Error handling
- `sonic.onMessageReceived(msg)` - Incoming OSC messages
- `sonic.onMessageSent(oscData)` - Outgoing OSC messages

**Common OSC commands:**
```javascript
sonic.send('/notify', 1);                              // Enable notifications
sonic.send('/s_new', 'synth-name', -1, 0, 0);         // Create synth
sonic.send('/n_set', 1000, 'freq', 440.0, 'amp', 0.5); // Set parameters
sonic.send('/n_free', 1000);                            // Free node
sonic.send('/b_allocRead', 0, 'sample.flac');          // Load audio buffer
```

See [SuperCollider Server Command Reference](https://doc.sccode.org/Reference/Server-Command-Reference.html) for the full OSC API.

## Browser Requirements

**Minimum browser versions:**
- Chrome/Edge 92+
- Firefox 79+
- Safari 15.2+

**Required features:**
- SharedArrayBuffer (requires COOP/COEP headers)
- AudioWorklet
- WebAssembly with threads

**Required HTTP headers:**
Your server must send these headers for SharedArrayBuffer support:

```
Cross-Origin-Opener-Policy: same-origin
Cross-Origin-Embedder-Policy: require-corp
Cross-Origin-Resource-Policy: cross-origin
```

See `example/server.rb` for a reference implementation.

## CDN Usage

SuperSonic cannot be loaded from a CDN. The core library must be self-hosted on your domain.

### Why Self-Hosting is Required

SuperSonic uses `SharedArrayBuffer` for real-time audio performance. Browsers require workers that use `SharedArrayBuffer` to come from the same origin as the page. Even with proper COOP/COEP headers, cross-origin workers with shared memory are blocked. This is a fundamental browser security requirement stemming from Spectre attack mitigation.

What this means:
- You cannot use `import { SuperSonic } from 'https://unpkg.com/supersonic/...'`
- You must download and self-host the core library on your own domain
- The npm packages exist for convenience but must be bundled and deployed to your server

### Synthdefs and Samples Can Use CDN

Pre-compiled synthdefs and audio samples can be loaded from CDNs. They're just data files, not workers.

```javascript
// Self-hosted core library
import { SuperSonic } from './dist/supersonic.js';

// CDN-hosted synthdefs and samples work fine
const sonic = new SuperSonic({
  workerBaseURL: './dist/workers/',  // Must be self-hosted
  wasmBaseURL: './dist/wasm/',       // Must be self-hosted
  sampleBaseURL: 'https://unpkg.com/supersonic-scsynth-samples@0.1.6/samples/',
  synthdefBaseURL: 'https://unpkg.com/supersonic-scsynth-synthdefs@0.1.6/synthdefs/'
});

await sonic.init();
await sonic.loadSynthDefs(['sonic-pi-beep', 'sonic-pi-tb303']);
```

### Hybrid Approach

Self-host the SuperSonic core (JS, WASM, workers) with COOP/COEP headers. Use CDN for synthdefs and samples to save bandwidth. See `example/simple-cdn.html` for a working example.

## Building from Source

**Prerequisites:**
- [Emscripten SDK](https://emscripten.org/docs/getting_started/downloads.html)
- [esbuild](https://esbuild.github.io/)

**Build:**
```bash
# Activate Emscripten
source ~/path/to/emsdk_env.sh

# Compile and bundle
./build.sh
```

Output goes to `dist/` directory (~1.5MB WASM + ~76KB JS + workers).

**Run demo:**
```bash
ruby example/server.rb
```

Open http://localhost:8002/demo.html

**Docker:**
```bash
docker build -t supersonic .
docker run --rm -it -p 8002:8002 supersonic
```

## File Structure

When building from source or using local files:

```
dist/
├── supersonic.js                 # Main entry point (ES module)
├── wasm/
│   └── scsynth-nrt.wasm          # Audio engine (~1.5MB)
└── workers/
    ├── scsynth_audio_worklet.js  # AudioWorklet processor
    ├── osc_in_worker.js          # OSC input handler
    ├── osc_out_prescheduler_worker.js # OSC pre-scheduler (timers & tag cancellation)
    └── debug_worker.js           # Debug logger
```

You must specify the paths to `workers/` and `wasm/` directories when creating a SuperSonic instance using the `workerBaseURL` and `wasmBaseURL` options.

## License

GPL v3 - This is a derivative work of SuperCollider

## Credits

Based on [SuperCollider](https://supercollider.github.io/) by James McCartney and the SuperCollider community. This AudioWorklet port was inspired by Hanns Holger Rutz who started the first port of scsynth to WASM and Dennis Scheiba who continued this work. Thank you to everyone in the SuperCollider community!
