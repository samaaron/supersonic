# CDN & Self-Hosting

Browser security means the core files must live on your server.

## Why Self-Hosting is Required

SuperSonic uses SharedArrayBuffer for real-time audio communication between JavaScript and the WASM engine. This requires workers to run on the same origin as your page.

Even with proper COOP/COEP headers, browsers block cross-origin workers from using SharedArrayBuffer.

**What this means:**

```javascript
// This won't work - cross-origin workers are blocked
import { SuperSonic } from 'https://cdn.example.com/supersonic.js';

// This works - same origin as your page
import { SuperSonic } from './supersonic.js';
```

## What Must Be Self-Hosted

These files must be served from your own domain:

- `supersonic.js` - Main entry point
- `workers/*.js` - All worker files
- `wasm/*.wasm` - WebAssembly files

## What Can Use CDN

Synthdefs and samples are just data files, not workers. They can be loaded from anywhere:

- `synthdefs/*.scsyndef` - Synth definitions
- `samples/*.flac` - Audio samples

## Hybrid Approach

The recommended setup: self-host the core, use CDN for assets.

```javascript
import { SuperSonic } from "supersonic-scsynth";

const baseURL = "/supersonic"; // Configure for your setup
const supersonic = new SuperSonic({
  // Must be self-hosted (workers use SharedArrayBuffer)
  workerBaseURL: `${baseURL}/workers/`,
  wasmBaseURL:   `${baseURL}/wasm/`,

  // Can use CDN (just data files)
  synthdefBaseURL: "https://unpkg.com/supersonic-scsynth-synthdefs@latest/synthdefs/",
  sampleBaseURL:   "https://unpkg.com/supersonic-scsynth-samples@latest/samples/"
});
```

This gives you:
- Small self-hosted footprint (~450KB JS + ~1.5MB WASM)
- CDN-cached synthdefs and samples (~34MB)

## Setting Up Self-Hosting

**1. Download the core files:**

```bash
curl -O https://samaaron.github.io/supersonic/supersonic-dist.zip
unzip supersonic-dist.zip
```

**2. Copy to your web server:**

```
your-site/
├── supersonic.js
├── workers/
│   ├── scsynth_audio_worklet.js
│   ├── osc_in_worker.js
│   ├── osc_out_prescheduler_worker.js
│   └── debug_worker.js
└── wasm/
    └── scsynth-nrt.wasm
```

**3. Configure headers:**

Your server must send COOP/COEP headers. See [Browser Setup](BROWSER_SETUP.md).

**4. Use in your code:**

```javascript
import { SuperSonic } from "supersonic-scsynth";

const baseURL = "/supersonic"; // Configure for your setup
const supersonic = new SuperSonic({
  workerBaseURL:   `${baseURL}/workers/`,
  wasmBaseURL:     `${baseURL}/wasm/`,
  synthdefBaseURL: `${baseURL}/synthdefs/`,
  sampleBaseURL:   `${baseURL}/samples/`
});
```

## Example: Complete Self-Hosted Setup

If you want everything local (no CDN at all):

```javascript
import { SuperSonic } from "supersonic-scsynth";

const baseURL = "/supersonic"; // Configure for your setup
const supersonic = new SuperSonic({
  workerBaseURL:   `${baseURL}/workers/`,
  wasmBaseURL:     `${baseURL}/wasm/`,
  synthdefBaseURL: `${baseURL}/synthdefs/`,
  sampleBaseURL:   `${baseURL}/samples/`
});

await supersonic.init();
await supersonic.loadSynthDef("sonic-pi-beep");
supersonic.send("/s_new", "sonic-pi-beep", -1, 0, 0, "note", 60);
```

## npm Packages

The npm packages exist for convenience, but remember they must be bundled and deployed to your server:

```bash
npm install supersonic-scsynth-bundle
```

Then copy the files from `node_modules/` to your public directory, or configure your bundler to include them.
