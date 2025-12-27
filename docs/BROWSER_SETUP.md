# Browser Setup

SuperSonic works in all modern browsers. Configuration requirements depend on which transport mode you use.

## Transport Modes

SuperSonic supports two transport modes:

| Mode | Headers Required | Latency | Use Case |
|------|------------------|---------|----------|
| `postMessage` (default) | None | Higher | CDN, simple hosting, getting started |
| `sab` | COOP/COEP | Lower | Production apps needing minimal latency |

### PostMessage Mode (Default)

Works everywhere with no special server configuration:

```javascript
const supersonic = new SuperSonic();  // postMessage is the default
await supersonic.init();
```

This mode uses MessagePort for communication. It's slightly higher latency but works on any static host (GitHub Pages, Netlify, Vercel, S3, etc.) without COOP/COEP headers.

### SAB Mode (SharedArrayBuffer)

For lower latency, use SAB mode - but this requires specific server headers:

```javascript
const supersonic = new SuperSonic({ mode: 'sab' });
await supersonic.init();
```

## Browser Requirements

**Minimum versions:**
- Chrome/Edge 92+
- Firefox 79+
- Safari 15.2+

**Required features:**
- AudioWorklet
- WebAssembly
- SharedArrayBuffer (SAB mode only)

## Server Headers (SAB Mode Only)

If using `mode: 'sab'`, your server must send these HTTP headers:

```
Cross-Origin-Opener-Policy: same-origin
Cross-Origin-Embedder-Policy: require-corp
```

### Why These Headers?

SharedArrayBuffer was disabled in browsers after the Spectre vulnerability was discovered. These headers re-enable it by isolating your page from other origins, preventing potential timing attacks.

Without these headers in SAB mode, you'll see errors like:
- `SharedArrayBuffer is not defined`
- `Cannot construct a SharedArrayBuffer`

**Solution:** Either add the headers (see below) or use the default `postMessage` mode.

### Example Server Configurations

**serve (npx)**

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

Then run:

```bash
npx serve
```

**Nginx**

```nginx
server {
    listen 8080;
    root /var/www/supersonic;

    add_header Cross-Origin-Opener-Policy same-origin;
    add_header Cross-Origin-Embedder-Policy require-corp;

    location ~ \.wasm$ {
        types { application/wasm wasm; }
    }
}
```

**Apache**

```apache
<IfModule mod_headers.c>
    Header set Cross-Origin-Opener-Policy "same-origin"
    Header set Cross-Origin-Embedder-Policy "require-corp"
</IfModule>

AddType application/wasm .wasm
```

**Express (Node.js)**

```javascript
const express = require('express');
const app = express();

app.use((req, res, next) => {
  res.setHeader('Cross-Origin-Opener-Policy', 'same-origin');
  res.setHeader('Cross-Origin-Embedder-Policy', 'require-corp');
  next();
});

app.use(express.static('public'));
app.listen(8080);
```

## User Interaction Requirement

Browsers require a user gesture (click, tap, keypress) before audio can play. This is a browser policy, not a SuperSonic limitation.

```javascript
document.getElementById('start-button').addEventListener('click', async () => {
  await supersonic.init();
  // Now audio will work
});
```

## Troubleshooting

### "SharedArrayBuffer is not defined"

You're using SAB mode but your server isn't sending COOP/COEP headers. Either:
1. Configure your server to send the headers (see above)
2. Or use the default `postMessage` mode which doesn't require headers

### Audio doesn't play

Make sure `init()` is called after a user interaction (button click, etc).

### "Cannot construct AudioContext"

The browser may be blocking audio. Some browsers require user interaction before creating an AudioContext.

### WASM fails to load

Check that:
1. The `.wasm` file is being served with `Content-Type: application/wasm`
2. The `wasmBaseURL` path is correct (or let it auto-detect from import path)
3. CORS headers allow loading from your domain

## Testing Locally

The easiest way to test locally:

```bash
cd example && npx serve
# Open http://localhost:3000/demo.html
```

No special headers needed - the default `postMessage` mode just works.

For SAB mode testing, use the `serve.json` configuration shown above.
