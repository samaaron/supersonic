# Installation

Welcome to the SuperSonic installation guide.

There are a few ways to add SuperSonic to your website:

|                              |                                                  |
|------------------------------|--------------------------------------------------|
| [CDN](#cdn)                  | Quick experiments, prototypes, getting started   |
| [npm](#npm)                  | JavaScript projects using bundlers               |
| [Self-Hosted](#self-hosted)  | Full control, offline use, production deployments|

## CDN

This is the simplest way to get started as there's nothing to install or configure. Just import SuperSonic directly from a CDN:

**unpkg**
```javascript
import { SuperSonic } from "https://unpkg.com/supersonic-scsynth@latest";
```

**jsDelivr**
```javascript
import { SuperSonic } from "https://cdn.jsdelivr.net/npm/supersonic-scsynth@latest";
```

This loads the client API and sets up paths to fetch assets from CDN. The WASM engine and workers are fetched when you call `init()`. Synthdefs and samples are fetched on demand when you call `loadSynthDef()` or `loadSample()`.

For production, consider pinning to a specific version:
```javascript
import { SuperSonic } from "https://unpkg.com/supersonic-scsynth@0.36.0";
```


## npm

If you're working within a JavaScript project that uses a bundler or Node.js tooling, you can install SuperSonic via npm:

```bash
npm install supersonic-scsynth
```

Then import it as you would any other module:

```javascript
import { SuperSonic } from "supersonic-scsynth";
```

When using a bundler, you'll likely need to configure where assets are loaded from. See the [Self-Hosted](#self-hosted) section below for configuration options, or point to CDN explicitly:

```javascript
const supersonic = new SuperSonic({
  coreBaseURL: "https://unpkg.com/supersonic-scsynth-core@latest/",
  synthdefBaseURL: "https://unpkg.com/supersonic-scsynth-synthdefs@latest/synthdefs/",
  sampleBaseURL: "https://unpkg.com/supersonic-scsynth-samples@latest/samples/"
});
```

### Package Options

SuperSonic is split into several packages to give you control over what you include:

| Package | Contains | License |
|---------|----------|---------|
| `supersonic-scsynth` | Client API only | MIT |
| `supersonic-scsynth-core` | WASM engine + workers | GPL-3.0 |
| `supersonic-scsynth-synthdefs` | 127 synth definitions | MIT |
| `supersonic-scsynth-samples` | 206 audio samples | CC0 |
| `supersonic-scsynth-bundle` | All of the above | Mixed |

When importing from CDN, most users just need `supersonic-scsynth` as the other packages are loaded from CDN automatically. When using npm with a bundler, you'll need to configure the asset URLs explicitly (see above).

s
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
├── synthdefs/         # 127 synth definitions
└── samples/           # 206 audio samples
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

SuperSonic supports two transport modes for communication between JavaScript and the AudioWorklet. The choice (slightly) affects internal jitter and which HTTP headers your server needs to send.

| Mode | Headers Required | Jitter | Use Case |
|------|------------------|---------|----------|
| `postMessage` | None | Higher | General purpose use |
| `sab` | COOP/COEP | Lower | Production apps needing minimal jitter |

### PostMessage Mode (Default)

This is the default mode and works everywhere with no special configuration:

```javascript
const supersonic = new SuperSonic({
  baseURL: "/supersonic/"  // postMessage is default
});
```

### SAB Mode (SharedArrayBuffer)

For lower internal jitter (due to post message delays between threads), you can use SAB mode which uses SharedArrayBuffer for direct communication between threads:

```javascript
const supersonic = new SuperSonic({
  baseURL: "/supersonic/",
  mode: "sab"
});
```

However, for this to work your server must send the following headers:

```
Cross-Origin-Opener-Policy: same-origin
Cross-Origin-Embedder-Policy: require-corp
```

See [Server Configuration](#server-configuration-sab-mode) below for examples of how to configure these for different hosting environments.


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

If you're using `mode: 'sab'` for lower latency, your server needs to send the COOP/COEP headers. Here are examples for common setups:

### serve (npx)

Create a `serve.json` file:

```json
{
  "headers": [
    {
      "source": "**/*",
      "headers": [
        { "key": "Cross-Origin-Opener-Policy", "value": "same-origin" },
        { "key": "Cross-Origin-Embedder-Policy", "value": "require-corp" }
      ]
    }
  ]
}
```

Then run: `npx serve`

### Nginx

```nginx
server {
    add_header Cross-Origin-Opener-Policy same-origin;
    add_header Cross-Origin-Embedder-Policy require-corp;

    location ~ \.wasm$ {
        types { application/wasm wasm; }
    }
}
```

### Apache

```apache
<IfModule mod_headers.c>
    Header set Cross-Origin-Opener-Policy "same-origin"
    Header set Cross-Origin-Embedder-Policy "require-corp"
</IfModule>
AddType application/wasm .wasm
```

### Express (Node.js)

```javascript
app.use((req, res, next) => {
  res.setHeader('Cross-Origin-Opener-Policy', 'same-origin');
  res.setHeader('Cross-Origin-Embedder-Policy', 'require-corp');
  next();
});
app.use(express.static('public'));
```


## Troubleshooting

### "SharedArrayBuffer is not defined"

You're using SAB mode without COOP/COEP headers. Either configure your server to send the headers (see above) or use the default `postMessage` mode.

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
