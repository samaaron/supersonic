# Installation

Welcome to the SuperSonic installation guide.

There are a few ways to add SuperSonic to your website:

|                              |                                                  |
|------------------------------|--------------------------------------------------|
| [CDN](#cdn)                  | Quick experiments, prototypes, getting started   |
| [npm](#npm)                  | JavaScript projects using bundlers               |
| [Self-Hosted](#self-hosted)  | Full control, offline use, production deployments|

## CDN

Nothing to install: import the client from a CDN and tell it where the
packages live. The engine, the AudioWorklet and the workers are fetched when
you call `init()`; synthdefs and samples on demand.

```javascript
import { SuperSonic } from "https://unpkg.com/supersonic-scsynth@0.82.0/dist/supersonic.js";

const CDN = "https://unpkg.com/";   // or "https://cdn.jsdelivr.net/npm/"
const supersonic = new SuperSonic({
  mode: "postMessage",   // a CDN cannot send the COOP/COEP headers the SAB transport needs
  baseURL:         CDN + "supersonic-scsynth@0.82.0/dist/",              // client, workers
  coreBaseURL:     CDN + "supersonic-scsynth-core@0.82.0/",              // engine wasm, AudioWorklet
  wasmBaseURL:     CDN + "supersonic-scsynth-core@0.82.0/wasm/",         // needed up to 0.81.0, see below
  synthdefBaseURL: CDN + "supersonic-scsynth-synthdefs@0.82.0/synthdefs/",
  sampleBaseURL:   CDN + "supersonic-scsynth-samples@0.82.0/samples/",
});
await supersonic.init();
```

Three things this recipe gets right that a shorter one does not:

- **Name the file, not the package.** jsDelivr serves a bare package URL's
  entry file without redirecting to its path, so the client's own relative
  imports (the lazily loaded chunks under `dist/chunks/`) resolve against
  the wrong base and 404. unpkg redirects, so either form works there; the
  full path works on both.
- **`coreBaseURL` is not optional on a CDN.** The client package ships no
  wasm; the engine lives in `supersonic-scsynth-core`.
- **`wasmBaseURL`, up to 0.81.0.** Those releases derive the wasm URL from
  `baseURL` and ignore `coreBaseURL`, so without it a CDN boot fails at
  `init()` with a 404 on `scsynth-nrt.wasm`. Later releases honour
  `coreBaseURL`; passing `wasmBaseURL` as well is harmless there.

Pin a version for anything you deploy; `@latest` moves.

### Package Options

SuperSonic is split into several packages to give you control over what you include:

| Package | Contains | License |
|---------|----------|---------|
| `supersonic-scsynth` | Client API + workers + metrics component | AGPL-3.0-or-later |
| `supersonic-scsynth-core` | WASM engine + AudioWorklet | AGPL-3.0-or-later |
| `supersonic-scsynth-synthdefs` | 119 Sonic Pi synth definitions, plus 11 test ones | MIT |
| `supersonic-scsynth-samples` | 207 audio samples | CC0 |
| `supersonic-scsynth-bundle` | All of the above | Mixed |

When importing from CDN, most users just need `supersonic-scsynth` as the other packages are loaded from CDN automatically. When using npm with a bundler, you'll need to configure the asset URLs explicitly (see above).

The `supersonic-scsynth` package also exports a metrics web component and CSS themes:

```javascript
import "supersonic-scsynth/metrics";           // <supersonic-metrics> custom element
import "supersonic-scsynth/metrics-dark.css";  // Dark theme
import "supersonic-scsynth/metrics-light.css"; // Light theme
```

See [Metrics Component](METRICS_COMPONENT.md) for usage details.

## Self-Hosted

If you'd like full control over the assets or need to run offline, you can download the pre-built distribution from [GitHub Releases](https://github.com/samaaron/supersonic/releases):

```bash
curl -LO https://github.com/samaaron/supersonic/releases/latest/download/supersonic.zip
unzip supersonic.zip
```

This gives you everything you need:

```
supersonic/
├── supersonic.js      # Main library
├── wasm/              # WebAssembly binaries
├── workers/           # Web Workers
├── synthdefs/         # 130 synth definitions
└── samples/           # 207 audio samples
```

Then import from your local path:

```javascript
import { SuperSonic } from "./supersonic/supersonic.js";
```

### Pointing to Your Assets

If your assets live at a different path, you can configure the base URL when creating your SuperSonic instance:

```javascript
const supersonic = new SuperSonic({
  baseURL: "/audio/supersonic/"
  // Derives: /audio/supersonic/workers/, /audio/supersonic/wasm/, etc.
});
```

You can also override individual paths if needed:

```javascript
const supersonic = new SuperSonic({
  workerBaseURL: "/my-workers/",
  wasmBaseURL: "/my-wasm/",
  synthdefBaseURL: "/my-synthdefs/",
  sampleBaseURL: "/my-samples/"
});
```

### Hybrid (Self-Host Core, CDN for Assets)

You can self-host the small core files while using CDN for the larger assets:

```javascript
const supersonic = new SuperSonic({
  baseURL: "/supersonic/",
  synthdefBaseURL: "https://unpkg.com/supersonic-scsynth-synthdefs@latest/synthdefs/",
  sampleBaseURL: "https://unpkg.com/supersonic-scsynth-samples@latest/samples/"
});
```


## Transport Modes

SuperSonic supports two transport modes: **postMessage** (default, works everywhere) and **SAB** (SharedArrayBuffer, lower latency but requires server headers).

The default `postMessage` mode needs no configuration. For SAB mode, set `mode: "sab"` and configure your server to send COOP/COEP headers.

See [Communication Modes](MODES.md) for a detailed comparison, server configuration examples, and technical details.


## Configuration Options

Beyond installation and transport mode, SuperSonic accepts additional options for debugging and tuning the scsynth engine:

```javascript
const supersonic = new SuperSonic({
  debug: true,  // Log scsynth output, OSC in/out to console
  scsynthOptions: {
    numBuffers: 4096,           // Max audio buffers (default: 1024)
    numAudioBusChannels: 256,   // Audio buses (default: 128)
    realTimeMemorySize: 16384   // RT memory in KB (default: 8192)
  }
});
```

See [API Reference](API.md) for all available options.


## Browser Requirements

SuperSonic requires a modern browser with the following features:

**Minimum versions:**
- Chrome/Edge 92+
- Firefox 79+
- Safari 15.2+

**Required features:**
- AudioWorklet
- WebAssembly
- SharedArrayBuffer (SAB mode only)

**User interaction required:** Browsers require a click, tap, or keypress before audio can play. Always call `init()` from a user interaction handler such as a button click.


## Server Configuration (SAB Mode)

If you're using `mode: 'sab'` for lower latency, your server needs to send COOP/COEP headers. See [Communication Modes](MODES.md#server-headers-for-sab-mode) for configuration examples for serve, Nginx, Apache, Express, and Vite.


## Troubleshooting

### "SharedArrayBuffer is not defined"

You're using SAB mode without COOP/COEP headers. Either configure your server to send the headers (see [Communication Modes](MODES.md#server-headers-for-sab-mode)) or use the default `postMessage` mode.

### Audio doesn't play

Make sure `init()` is called after a user interaction (button click, tap, keypress).

### WASM fails to load

Check that:
1. The `.wasm` file is served with `Content-Type: application/wasm`
2. CORS headers allow loading from your domain


## Testing Locally

For local development:

```bash
cd example && npx serve
# Open http://localhost:3000/demo.html
```

The default `postMessage` mode works without any special headers, so you can get started immediately.


## Next: Quick Start

Now that you have SuperSonic installed, head to the [Quick Start](QUICKSTART.md) to make your first sound.
