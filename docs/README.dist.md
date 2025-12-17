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
- Live demo: https://sonic-pi.net/supersonic/demo.html

## Support

Please consider joining the community of supporters enabling Sam's work on creative coding projects:

- Patreon: https://patreon.com/samaaron
- GitHub Sponsors: https://github.com/sponsors/samaaron

## License

GPL v3 - Source code available at https://github.com/samaaron/supersonic

This is a derivative work of [SuperCollider](https://supercollider.github.io/) by James McCartney and the SuperCollider community.
