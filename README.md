# SuperSonic

> **Warning - Super Alpha Status**: SuperSonic is currently in active development. The API is likely to change between releases. Feedback welcome!

A WebAssembly port of SuperCollider's scsynth audio synthesis engine for the browser. Runs in an AudioWorklet for real-time, high-priority audio processing with full OSC API support.

## Quick Start

```html
<script type="module">
  import { SuperSonic } from 'https://unpkg.com/supersonic-scsynth@latest';

  const sonic = new SuperSonic({
    sampleBaseURL: 'https://unpkg.com/supersonic-scsynth-samples@latest/samples/',
    synthdefBaseURL: 'https://unpkg.com/supersonic-scsynth-synthdefs@latest/synthdefs/'
  });

  await sonic.init();

  // Load a synthdef
  await sonic.loadSynthDefs(['sonic-pi-beep']);

  // Trigger the synth
  sonic.send('/s_new', 'sonic-pi-beep', -1, 0, 1, 'note', 60);

  // Load and play a sample
  sonic.send('/b_allocRead', 0, 'bd_haus.flac');
  sonic.send('/s_new', 'sonic-pi-basic_mono_player', -1, 0, 1, 'buf', 0);
</script>
```

**Note:** Requires specific HTTP headers (COOP/COEP) for SharedArrayBuffer support. See [Browser Requirements](#browser-requirements) below.

## Installation

**Via npm:**
```bash
# Core engine only (~450KB)
npm install supersonic-scsynth

# Everything (engine + synthdefs + samples)
npm install supersonic-scsynth-bundle
```

**Via CDN:**
```javascript
import { SuperSonic } from 'https://unpkg.com/supersonic-scsynth@latest';
```

**Pre-built distribution:**
Download the 'nightly' build (~35MB with all synthdefs and samples):
https://samaaron.github.io/supersonic/supersonic-dist.zip

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
  sampleBaseURL: 'https://unpkg.com/supersonic-scsynth-samples@latest/samples/',
  synthdefBaseURL: 'https://unpkg.com/supersonic-scsynth-synthdefs@latest/synthdefs/',
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
sonic.send('/s_new', 'synth-name', -1, 0, 1);         // Create synth
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
    ├── osc_out_worker.js         # OSC output handler
    └── debug_worker.js           # Debug logger
```

The engine expects these files at `./dist/` relative to your HTML. Paths are currently not configurable.

## License

GPL v3 - This is a derivative work of SuperCollider

## Credits

Based on [SuperCollider](https://supercollider.github.io/) by James McCartney and the SuperCollider community. This AudioWorklet port was inspired by Hanns Holger Rutz who started the first port of scsynth to WASM and Dennis Scheiba who continued this work. Thank you to everyone in the SuperCollider community!
