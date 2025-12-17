# Browser Setup

SuperSonic requires some specific browser features and server configuration to work properly.

## Browser Requirements

**Minimum versions:**
- Chrome/Edge 92+
- Firefox 79+
- Safari 15.2+

**Required features:**
- SharedArrayBuffer
- AudioWorklet
- WebAssembly with threads

## Server Headers

Your server must send these HTTP headers for SharedArrayBuffer to work:

```
Cross-Origin-Opener-Policy: same-origin
Cross-Origin-Embedder-Policy: require-corp
Cross-Origin-Resource-Policy: cross-origin
```

### Why These Headers?

SharedArrayBuffer was disabled in browsers after the Spectre vulnerability was discovered. These headers re-enable it by isolating your page from other origins, preventing potential timing attacks.

Without these headers, you'll see errors like:
- `SharedArrayBuffer is not defined`
- `Cannot construct a SharedArrayBuffer`

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
    add_header Cross-Origin-Resource-Policy cross-origin;

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
    Header set Cross-Origin-Resource-Policy "cross-origin"
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
  res.setHeader('Cross-Origin-Resource-Policy', 'cross-origin');
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

Your server isn't sending the required COOP/COEP headers. Check your server configuration.

### Audio doesn't play

Make sure `init()` is called after a user interaction (button click, etc).

### "Cannot construct AudioContext"

The browser may be blocking audio. Some browsers require user interaction before creating an AudioContext.

### WASM fails to load

Check that:
1. The `.wasm` file is being served with `Content-Type: application/wasm`
2. The `wasmBaseURL` path is correct
3. CORS headers allow loading from your domain

## Testing Locally

The easiest way to test locally is using npx serve:

```bash
cd example && npx serve
# Open http://localhost:3000/demo.html
```

Or use Docker:

```bash
docker build -t supersonic .
docker run --rm -it -p 8002:8002 supersonic
```
