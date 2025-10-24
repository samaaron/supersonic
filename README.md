# SuperSonic

> **Warning - Super Alpha Status**: SuperSonic is currently in active development (v0.1.0). The API is likely to change between releases. Feedback welcome! Also note that loading audio files is not supported yet but planned for the immediate future.

This is a WebAssembly port of SuperCollider's highly flexible and powerful synthesis engine scsynth.

SuperSonic's scsynth engine runs within a web AudioWorklet giving it access to a high-priority audio thread for real-time browser-based audio synthesis.

The main API for SuperSonic is scsynth's OSC API with support for immediate and scheduled execution of OSC messages and bundles. It's also possible to register a handler to receive OSC replies from scsynth in addition to debug messages that would normally have been printed to stdout.

Note: SuperSonic uses a SharedBuffer to send and receive OSC messages from scsynth which requires specific COOP/COEP headers to be set in your web browser (see below).


## Quick Start

If you are familiar with Docker, you can build and run the example using the following commands:

```bash
docker build -t supersonic .
docker run --rm -it -p 8002:8002 supersonic
```

Or to build and run locally follow the instructions below.

### 1. Build

**Prerequisites:**
- [Emscripten SDK](https://emscripten.org/docs/getting_started/downloads.html)
- [esbuild](https://esbuild.github.io/)

```bash
# Activate Emscripten
source ~/path/to/emsdk_env.sh

# Build (compiles C++ to WASM and bundles JavaScript)
./build.sh
```

Outputs to `dist/` directory (~1.5MB WASM + ~80KB JS).

### 2. Serve Demo

Start the basic Ruby webserver with the correct headers:

```bash
ruby example/server.rb
```

Open: http://localhost:8002/demo.html

## Browser Requirements

**Minimum Versions:**
- Chrome/Edge 92+
- Firefox 79+
- Safari 15.2+

**Required Features:**
- SharedArrayBuffer (requires COOP/COEP headers - see below)
- AudioWorklet
- WebAssembly with threads support

**Required HTTP Headers:**

Important: your server must send these headers for SharedArrayBuffer to work:

```
Cross-Origin-Opener-Policy: same-origin
Cross-Origin-Embedder-Policy: require-corp
Cross-Origin-Resource-Policy: cross-origin
```

See `example/server.rb` for a reference implementation.

## Basic Usage

### Minimal Example

```javascript
import { SuperSonic } from './dist/supersonic.js';

const sonic = new SuperSonic();
await sonic.init();

sonic.send('/notify', 1);
```

See `example/demo.html` for a complete working example.

### API Reference

**SuperSonic Class:**
- `new SuperSonic()` - Create instance
- `async init()` - Initialize audio engine
- `send(address, ...args)` - Send OSC message (auto-detects types)
- `sendOSC(oscBytes, options)` - Send pre-encoded OSC bytes
- `onInitialized` - Callback when ready
- `onError(error)` - Error callback
- `onMessageReceived(msg)` - Incoming OSC message callback
- `onMessageSent(oscData)` - Outgoing OSC message callback

**Sending OSC Messages:**
```javascript
// Types are auto-detected: strings → 's', integers → 'i', floats → 'f'
sonic.send('/notify', 1);
sonic.send('/s_new', 'sonic-pi-beep', -1, 0, 0);
sonic.send('/n_set', 1000, 'freq', 440.0, 'amp', 0.5);
```

**Common OSC Commands:**
- `/d_recv` - Load synth definition
- `/s_new` - Create synth
- `/n_free` - Free node
- `/n_set` - Set node parameters
- `/notify` - Enable server notifications

See [SuperCollider Server Command Reference](https://doc.sccode.org/Reference/Server-Command-Reference.html) for full OSC API.

## Integration Guide

### Required Files

To integrate into your project, copy these from `dist/`:

```
dist/
├── supersonic.js                 # Main entry point (ES module)
├── wasm/
│   └── scsynth-nrt.wasm          # Audio engine (~1.5MB)
├── workers/
│   ├── osc_in_worker.js          # OSC input handling
│   ├── osc_out_worker.js         # OSC output handling
│   ├── debug_worker.js           # Debug logging
│   └── scsynth_audio_worklet.js  # AudioWorklet processor
├── lib/
│   ├── ring_buffer.js            # SharedArrayBuffer ring buffer
│   └── scsynth_osc.js            # OSC orchestration
└── etc/
    └── synthdefs.js              # Pre-compiled synth definitions
                                  # from Sonic Pi
```

### Path Requirements

**Default Paths:**

By default, Supersonic expects files to be served from `./dist/` relative to your HTML file:
- WASM: `./dist/wasm/scsynth-nrt.wasm`
- AudioWorklet: `./dist/workers/scsynth_audio_worklet.js`
- Workers: `./dist/workers/osc_out_worker.js`, `osc_in_worker.js`, `debug_worker.js`

**Custom Paths:**

You can configure WASM and AudioWorklet paths when creating a SuperSonic instance:

```javascript
const sonic = new SuperSonic();
sonic.config.wasmUrl = '/custom/path/scsynth-nrt.wasm';
sonic.config.workletUrl = '/custom/path/scsynth_audio_worklet.js';
await sonic.init();
```

**Note:** Worker paths (`osc_out_worker.js`, `osc_in_worker.js`, `debug_worker.js`) are currently hardcoded to `./dist/workers/` and cannot be configured. Keep these files in the default location.


## License

GPL v3 - This is a derivative work of SuperCollider

## Credits

Based on [SuperCollider](https://supercollider.github.io/) by James McCartney and the SuperCollider community. This AudioWorklet port was massively inspired and motivated by
Hanns Holger Rutz who started the first port of scsynth to WASM and Dennis Scheiba who took the baton and continued this great work. Thank-you also to everyone in the SuperCollider community, you're all beautiful people.
