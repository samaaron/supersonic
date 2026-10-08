# Installation

Welcome to the SuperSonic installation guide.

This guide is for the browser. SuperSonic also runs natively, as a server, and inside the BEAM as a NIF: see the [Native Guide](NATIVE.md) and the [NIF Guide](NIF.md) for those.

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
import { SuperSonic } from "https://unpkg.com/supersonic-scsynth@0.89.0/dist/supersonic.js";

const CDN = "https://unpkg.com/";   // or "https://cdn.jsdelivr.net/npm/"
const supersonic = new SuperSonic({
  baseURL:         CDN + "supersonic-scsynth@0.89.0/dist/",              // client, workers
  coreBaseURL:     CDN + "supersonic-scsynth-core@0.89.0/",              // engine wasm, AudioWorklet
  wasmBaseURL:     CDN + "supersonic-scsynth-core@0.89.0/wasm/",         // needed up to 0.81.0, see below
  synthdefBaseURL: CDN + "supersonic-scsynth-synthdefs@0.89.0/synthdefs/",
  sampleBaseURL:   CDN + "supersonic-scsynth-samples@0.89.0/samples/",
});

// Browsers start audio only after a click, tap or keypress
document.querySelector("button").onclick = () => supersonic.init();
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

The transport is chosen for you: SAB when your page is cross-origin isolated, postMessage when it is not (see [Transport Modes](#transport-modes)). Loading from a CDN works in both.

### Package Options

SuperSonic is split into several packages to give you control over what you include:

| Package | Contains | License |
|---------|----------|---------|
| `supersonic-scsynth` | Client API + workers + metrics component | AGPL-3.0-or-later |
| `supersonic-scsynth-core` | WASM engine + AudioWorklet, and the Web MIDI and gamepad modules | AGPL-3.0-or-later |
| `supersonic-scsynth-synthdefs` | Sonic Pi's synth definitions, plus a few the test suite uses | MIT |
| `supersonic-scsynth-samples` | Sonic Pi's audio samples | CC0 |
| `supersonic-scsynth-bundle` | All of the above | Mixed |

Nothing is fetched from a package you have not named. You always need the client and the core: `baseURL` (or `workerBaseURL`) says where the client's files are, and `coreBaseURL` where the core's are. Name `synthdefBaseURL` and `sampleBaseURL` if you load synthdefs and samples by name.

The `supersonic-scsynth` package also exports a metrics web component and CSS themes:

```javascript
import "supersonic-scsynth/metrics";           // <clockwork-metrics> custom element
import "supersonic-scsynth/metrics-dark.css";  // Dark theme
import "supersonic-scsynth/metrics-light.css"; // Light theme
```

See [Metrics Component](METRICS_COMPONENT.md) for usage details.

## npm

```bash
npm install supersonic-scsynth supersonic-scsynth-core
```

Import the client, and say where you serve the files it loads at run time:

```javascript
import { SuperSonic } from "supersonic-scsynth";

const supersonic = new SuperSonic({
  workerBaseURL: "/supersonic/workers/",   // node_modules/supersonic-scsynth/dist/workers/
  coreBaseURL:   "/supersonic-core/",      // node_modules/supersonic-scsynth-core/
});
```

Your bundler bundles the client, but not the workers, the AudioWorklet or the engine: those are loaded by URL when you call `init()`. Copy them to your static files - here, `node_modules/supersonic-scsynth/dist/workers/` to `/supersonic/workers/`, and `node_modules/supersonic-scsynth-core/` (its `wasm/` and `workers/` directories) to `/supersonic-core/`.

Synthdefs and samples are the same: install `supersonic-scsynth-synthdefs` and `supersonic-scsynth-samples` and serve their `synthdefs/` and `samples/` directories, or take them from a CDN (see [Hybrid](#hybrid-self-host-core-cdn-for-assets)).

## Self-Hosted

If you'd like full control over the assets or need to run offline, you can download the pre-built distribution from [GitHub Releases](https://github.com/samaaron/supersonic/releases):

```bash
curl -LO https://github.com/samaaron/supersonic/releases/latest/download/supersonic.zip
unzip supersonic.zip
```

This gives you everything you need:

```
supersonic/
├── supersonic.js          # The client
├── chunks/                # Parts of the client loaded on demand (MIDI, gamepad)
├── workers/               # Web Workers and the AudioWorklet
├── wasm/                  # The engine, and the MIDI and gamepad modules
├── synthdefs/             # Sonic Pi synth definitions
├── samples/               # Sonic Pi samples
├── metrics_component.js   # <clockwork-metrics>, with metrics-dark.css and metrics-light.css
├── osc_channel.js         # OscChannel on its own, for an AudioWorklet
├── midi/, gamepad/        # The MIDI and gamepad modules as wasm-bindgen packages
├── ...                    # The client's other modules (osc_fast.js and more)
└── README.md
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

The AudioWorklet, `clockwork_audio_worklet.js`, is looked for in `coreBaseURL`'s `workers/` directory, or in `workerBaseURL` when there is no `coreBaseURL` or `baseURL`.

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

SuperSonic has two transport modes: **SAB** (SharedArrayBuffer, lower latency) and **postMessage** (works everywhere).

Unless you say otherwise, SuperSonic uses SAB when the page is cross-origin isolated - its server sends the COOP/COEP headers - and postMessage when it is not. Set `mode: "sab"` or `mode: "postMessage"` to choose for yourself. `supersonic.mode` tells you which one is in use.

See [Communication Modes](MODES.md) for a detailed comparison, server configuration examples, and technical details.


## Configuration Options

Beyond installation and transport mode, SuperSonic accepts additional options for debugging and tuning the scsynth engine:

```javascript
const supersonic = new SuperSonic({
  debug: true,  // Log scsynth output, OSC in/out to console
  scsynthOptions: {
    numBuffers: 4096,           // Max audio buffers (default: 1024)
    numAudioBusChannels: 2048,  // Audio buses (default: 1024)
    realTimeMemorySize: 16384   // RT memory in KB (default: 8192)
  }
});
```

See [API Reference](API.md) for all available options.

### Web MIDI and Gamepads

Both are off by default: some browsers ask the user's permission for MIDI, and a page that never uses it should not ask. Turn them on with `midi: true` and `gamepad: true`, and they come up during `init()`. To ask for MIDI later, from the gesture that wants it, call `enableMidi()` after `init()`.

`supersonic.midi` and `supersonic.gamepad` are the managers, or `null` when they are off or could not come up. If one was asked for and could not come up, the engine boots without it, emits `'error'`, and `supersonic.midiError` or `supersonic.gamepadError` says why.

Their wasm modules are fetched from the core package's `wasm/` directory, and only when they are turned on.


## Browser Requirements

SuperSonic needs:

- **A secure context.** Serve your page over HTTPS, or from `localhost` while developing. Browsers offer AudioWorklet only there.
- **AudioWorklet and Web Workers.**
- **WebAssembly with SIMD, exception handling and shared memory.** The engine is built with all three.
- **SharedArrayBuffer and cross-origin isolation** - SAB mode only.

**User interaction required:** Browsers require a click, tap, or keypress before audio can play. Always call `init()` from a user interaction handler such as a button click.


## Server Configuration (SAB Mode)

SAB mode needs your page to be cross-origin isolated: your server sends COOP/COEP headers. See [Communication Modes](MODES.md#server-headers-for-sab-mode) for configuration examples for serve, Nginx, Apache, Express, and Vite.

The headers belong to your page's server, not to the CDN. The client loads cross-origin workers and the AudioWorklet from blob URLs, so SuperSonic's files can still come from a CDN that sends CORS headers (unpkg and jsDelivr do).


## Troubleshooting

### "Missing required features for sab mode"

You set `mode: "sab"` on a page that is not cross-origin isolated. Either configure your server to send the headers (see [Communication Modes](MODES.md#server-headers-for-sab-mode)) or leave `mode` out, and SuperSonic uses postMessage.

### "Missing required features for postMessage mode: audioWorklet"

The page is not a secure context: it was served over plain HTTP from an address other than `localhost`. Serve it over HTTPS.

### "Clockwork needs to be told where its files are"

The constructor was given no way to find the workers (`baseURL` or `workerBaseURL`) or the engine (`baseURL`, `coreBaseURL` or `wasmBaseURL`).

### Audio doesn't play

Make sure `init()` is called after a user interaction (button click, tap, keypress).

### WASM fails to load

Check that:
1. The network tab shows no 404 for `scsynth-nrt.wasm`. On a CDN, `coreBaseURL` must point at `supersonic-scsynth-core`.
2. CORS headers allow loading from your domain, if the files are on another one.

The engine's wasm is fetched as bytes, so its `Content-Type` does not matter. The MIDI and gamepad modules load faster served as `application/wasm`, and fall back, with a console warning, when they are not.


## Testing Locally

From a clone of the repository, after building (`scripts/build-web.sh`):

```bash
node scripts/serve-demo.mjs
# Open http://localhost:8080/example/demo.html
```

The server sends the COOP/COEP headers, so the examples run in SAB mode.


## Next: Quick Start

Now that you have SuperSonic installed, head to the [Quick Start](QUICKSTART.md) to make your first sound.
