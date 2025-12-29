# Deployment

SuperSonic works directly from CDN with zero configuration, or can be self-hosted.

## Quick Start (CDN)

The simplest way - just import from a CDN such as unpkg:

```html
<script type="module">
  import { SuperSonic } from "https://unpkg.com/supersonic-scsynth@latest";

  document.getElementById('play').onclick = async () => {
    const supersonic = new SuperSonic();
    await supersonic.init();
    await supersonic.loadSynthDef("sonic-pi-beep");
    supersonic.send("/s_new", "sonic-pi-beep", -1, 0, 0, "note", 60);
  };
</script>
```

## Deployment Options

### CDN (Recommended for Getting Started)

**unpkg**
```javascript
import { SuperSonic } from "https://unpkg.com/supersonic-scsynth@latest";
```

**jsDelivr**
```javascript
import { SuperSonic } from "https://cdn.jsdelivr.net/npm/supersonic-scsynth@latest";
```

For production, pin to a specific version:
```javascript
import { SuperSonic } from "https://unpkg.com/supersonic-scsynth@0.21.6";
```

### Self-Hosted

Download from [GitHub Releases](https://github.com/samaaron/supersonic/releases):

```bash
curl -LO https://github.com/samaaron/supersonic/releases/latest/download/supersonic.zip
unzip supersonic.zip
```

Directory structure:
```
your-site/
├── supersonic/
│   ├── supersonic.js
│   ├── workers/
│   ├── wasm/
│   ├── synthdefs/
│   └── samples/
```

Import and paths auto-detect:
```javascript
import { SuperSonic } from "./supersonic/supersonic.js";
const supersonic = new SuperSonic();
```

Or configure explicitly:
```javascript
const supersonic = new SuperSonic({
  baseURL: "/supersonic/"
});
```

### Hybrid (Self-Host Core, CDN for Assets)

Self-host the small core files, use CDN for large assets:

```javascript
const supersonic = new SuperSonic({
  baseURL: "/supersonic/",
  synthdefBaseURL: "https://unpkg.com/supersonic-scsynth-synthdefs@latest/synthdefs/",
  sampleBaseURL: "https://unpkg.com/supersonic-scsynth-samples@latest/samples/"
});
```

### npm

```bash
npm install supersonic-scsynth           # Core library
npm install supersonic-scsynth-synthdefs # Synth definitions
npm install supersonic-scsynth-samples   # Audio samples
```

## Transport Modes

SuperSonic supports two transport modes:

| Mode | Headers Required | Latency | Use Case |
|------|------------------|---------|----------|
| `postMessage` (default) | None | Higher | CDN, simple hosting, getting started |
| `sab` | COOP/COEP | Lower | Production apps needing minimal latency |

### PostMessage Mode (Default)

Works everywhere with no special configuration:

```javascript
const supersonic = new SuperSonic();  // postMessage is default
```

Uses MessagePort for communication. Works on any static host (GitHub Pages, Netlify, Vercel, S3, etc.).

### SAB Mode (SharedArrayBuffer)

For lower latency, use SAB mode:

```javascript
const supersonic = new SuperSonic({ mode: 'sab' });
```

Requires COOP/COEP headers (see Server Configuration below).

## Browser Requirements

**Minimum versions:**
- Chrome/Edge 92+
- Firefox 79+
- Safari 15.2+

**Required features:**
- AudioWorklet
- WebAssembly
- SharedArrayBuffer (SAB mode only)

**User interaction required:** Browsers require a click/tap/keypress before audio can play. Always call `init()` from a button handler.

## Server Configuration (SAB Mode Only)

If using `mode: 'sab'`, your server must send these HTTP headers:

```
Cross-Origin-Opener-Policy: same-origin
Cross-Origin-Embedder-Policy: require-corp
```

### serve (npx)

Create `serve.json`:
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

Then: `npx serve`

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

You're using SAB mode without COOP/COEP headers. Either:
1. Configure your server to send the headers (see above)
2. Use the default `postMessage` mode

### Audio doesn't play

Make sure `init()` is called after a user interaction (button click, etc).

### WASM fails to load

Check that:
1. The `.wasm` file is served with `Content-Type: application/wasm`
2. CORS headers allow loading from your domain

## Testing Locally

```bash
cd example && npx serve
# Open http://localhost:3000/demo.html
```

No special headers needed - the default `postMessage` mode just works.
