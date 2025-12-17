# SuperSonic {{VERSION}}

SuperCollider's powerful scsynth audio synthesis engine running in the browser.

## Quick Start

```javascript
import { SuperSonic } from '/supersonic/supersonic.js';

const sonic = new SuperSonic({ baseURL: '/supersonic/' });
await sonic.init();
```

## Required Server Headers

Your server must send these headers for SharedArrayBuffer to work:

```
Cross-Origin-Opener-Policy: same-origin
Cross-Origin-Embedder-Policy: require-corp
```

Without these headers you'll see: `SharedArrayBuffer is not defined`

## More Info

- Full documentation: https://github.com/samaaron/supersonic
- Live demo: https://samaaron.github.io/supersonic/demo.html
