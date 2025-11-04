# SuperSonic

> **Warning - Super Alpha Status**: SuperSonic is currently in active development (v0.1.0). The API is likely to change between releases. Feedback welcome!

This is a WebAssembly port of SuperCollider's highly flexible and powerful synthesis engine scsynth.

SuperSonic's scsynth engine runs within a web AudioWorklet giving it access to a high-priority audio thread for real-time browser-based audio synthesis.

The main API for SuperSonic is scsynth's OSC API with support for immediate and scheduled execution of OSC messages and bundles. It's also possible to register a handler to receive OSC replies from scsynth in addition to debug messages that would normally have been printed to stdout.

Note: SuperSonic uses a SharedBuffer to send and receive OSC messages from scsynth which requires specific COOP/COEP headers to be set in your web browser (see below).


## Installation

### Option 1: npm Package (Recommended)

```bash
# Core engine only (~2MB)
npm install supersonic-scsynth

# Or install everything (engine + synthdefs + samples)
npm install supersonic-scsynth-bundle
```

### Option 2: CDN (No build required)

```html
<script type="module">
  import { SuperSonic } from 'https://unpkg.com/supersonic-scsynth@0.1.0';

  const sonic = new SuperSonic({
    audioBaseURL: 'https://unpkg.com/supersonic-scsynth-samples@0.1.0/samples/'
  });

  await sonic.init();

  // Load synthdefs from CDN
  await sonic.loadSynthDefs(
    ['sonic-pi-beep'],
    'https://unpkg.com/supersonic-scsynth-synthdefs@0.1.0/synthdefs/'
  );
</script>
```

### Option 3: Pre-built Distribution

The 'nightly' (i.e. for every new commit) pre-built distribution files are published here:

https://samaaron.github.io/supersonic/supersonic-dist.zip

This includes:
- Core engine (WASM + JS)
- All 120 synthdefs
- All 206 samples
- Size: ~35MB

## Package Structure

SuperSonic is published as multiple npm packages to keep the core engine small:

### Core Package
- **`supersonic-scsynth`** (~2MB) - The WebAssembly scsynth engine
  - GPL-3.0-or-later license

### Resource Packages
- **`supersonic-scsynth-synthdefs`** (~67KB) - All 120 Sonic Pi synthdefs
  - MIT license
  - From [Sonic Pi](https://github.com/sonic-pi-net/sonic-pi)

- **`supersonic-scsynth-samples`** (~34MB) - All 206 Sonic Pi samples
  - CC0-1.0 license (public domain)
  - Categories: ambient, bass, drums, electronic, glitch, guitar, hi-hats, loops, percussion, snares, tabla, vinyl, and more
  - From [Sonic Pi](https://github.com/sonic-pi-net/sonic-pi)

### Convenience Package
- **`supersonic-scsynth-bundle`** - Includes core + synthdefs + all samples
  - Install this to get everything in one command

## Building from Source

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

### Minimal Example (Core Only)

```javascript
import { SuperSonic } from './dist/supersonic.js';

const sonic = new SuperSonic();
await sonic.init();

sonic.send('/notify', 1);
```

### With Synthdefs and Samples (npm/CDN)

```javascript
import { SuperSonic } from 'https://unpkg.com/supersonic-scsynth@0.1.0';

// Configure sample path (required for buffer loading)
const sonic = new SuperSonic({
  audioBaseURL: 'https://unpkg.com/supersonic-scsynth-samples@0.1.0/samples/'
});

await sonic.init();

// Load synthdefs (baseUrl is required)
await sonic.loadSynthDefs(
  ['sonic-pi-beep', 'sonic-pi-tb303'],
  'https://unpkg.com/supersonic-scsynth-synthdefs@0.1.0/synthdefs/'
);

// Play a synth
sonic.send('/s_new', 'sonic-pi-beep', -1, 0, 1, 'note', 60);

// Load and play a sample
await sonic.allocReadBuffer(0, 'bd_haus.flac');
sonic.send('/s_new', 'sonic-pi-basic_mono_player', -1, 0, 1, 'buf', 0);
```

See `example/demo.html` for a complete working example.

### API Reference

**SuperSonic Class:**
- `new SuperSonic(options)` - Create instance
  - `options.audioBaseURL` - Base URL for sample files (required for buffer loading)
  - `options.audioPathMap` - Custom mapping of sample names to URLs
- `async init()` - Initialize audio engine
- `async loadSynthDefs(names, baseUrl)` - Load synth definitions (baseUrl required)
- `async allocReadBuffer(bufnum, filename)` - Load audio file into buffer
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

### Recommended: npm Packages

Install the core package and any resources you need:

```bash
# Core engine
npm install supersonic-scsynth

# Synthdefs (optional)
npm install supersonic-scsynth-synthdefs

# Samples (optional)
npm install supersonic-scsynth-samples

# Or everything at once
npm install supersonic-scsynth-bundle
```

Then use via CDN in your HTML:

```html
<script type="module">
  import { SuperSonic } from 'https://unpkg.com/supersonic-scsynth@0.1.0';

  const sonic = new SuperSonic({
    audioBaseURL: 'https://unpkg.com/supersonic-scsynth-samples@0.1.0/samples/'
  });

  await sonic.init();
  await sonic.loadSynthDefs(
    ['sonic-pi-beep'],
    'https://unpkg.com/supersonic-scsynth-synthdefs@0.1.0/synthdefs/'
  );
</script>
```

### Alternative: Manual File Integration

If you're building from source or need local files, copy these from `dist/`:

```
dist/
├── supersonic.js                 # Main entry point (ES module)
├── wasm/
│   └── scsynth-nrt.wasm          # Audio engine (~1.5MB)
└── workers/
    ├── osc_in_worker.js          # OSC input handling
    ├── osc_out_worker.js         # OSC output handling
    ├── debug_worker.js           # Debug logging
    └── scsynth_audio_worklet.js  # AudioWorklet processor
```

Resources (synthdefs/samples) are available separately via npm packages.

### Path Configuration

**Required Configuration:**

Sample and synthdef paths must be explicitly configured:

```javascript
const sonic = new SuperSonic({
  audioBaseURL: 'https://unpkg.com/supersonic-scsynth-samples@0.1.0/samples/'
});

await sonic.loadSynthDefs(
  ['sonic-pi-beep'],
  'https://unpkg.com/supersonic-scsynth-synthdefs@0.1.0/synthdefs/'
);
```

**Engine Paths (Default):**

The core engine expects these files relative to your HTML:
- WASM: `./dist/wasm/scsynth-nrt.wasm`
- AudioWorklet: `./dist/workers/scsynth_audio_worklet.js`
- Workers: `./dist/workers/osc_out_worker.js`, `osc_in_worker.js`, `debug_worker.js`

**Custom Engine Paths:**

```javascript
const sonic = new SuperSonic();
sonic.config.wasmUrl = '/custom/path/scsynth-nrt.wasm';
sonic.config.workletUrl = '/custom/path/scsynth_audio_worklet.js';
await sonic.init();
```

**Note:** Worker paths are currently hardcoded to `./dist/workers/` and cannot be configured.


## License

GPL v3 - This is a derivative work of SuperCollider

## Credits

Based on [SuperCollider](https://supercollider.github.io/) by James McCartney and the SuperCollider community. This AudioWorklet port was massively inspired and motivated by
Hanns Holger Rutz who started the first port of scsynth to WASM and Dennis Scheiba who took the baton and continued this great work. Thank-you also to everyone in the SuperCollider community, you're all beautiful people.
