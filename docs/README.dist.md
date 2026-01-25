# SuperSonic {{VERSION}}

SuperCollider's powerful **scsynth** audio synthesis engine running in the browser as an AudioWorklet.

## Quick Start

```javascript
import { SuperSonic } from './supersonic/supersonic.js';

const supersonic = new SuperSonic({
  baseURL: './supersonic/'
});
await supersonic.init();
await supersonic.loadSynthDef('sonic-pi-beep');
supersonic.send('/s_new', 'sonic-pi-beep', -1, 0, 0, 'note', 60);
```

## Transport Modes

SuperSonic supports two transport modes:

| Mode | Headers Required | Latency | Use Case |
|------|------------------|---------|----------|
| `postMessage` (default) | None | Higher | Simple hosting, getting started |
| `sab` | COOP/COEP | Lower | Production apps needing minimal latency |

### PostMessage Mode (Default)

Works everywhere with no special configuration:

```javascript
const supersonic = new SuperSonic({
  baseURL: './supersonic/'  // postMessage is default
});
```

### SAB Mode (SharedArrayBuffer)

For lower latency, use SAB mode with required headers:

```javascript
const supersonic = new SuperSonic({
  baseURL: './supersonic/',
  mode: 'sab'
});
```

Your server must send these headers:
```
Cross-Origin-Opener-Policy: same-origin
Cross-Origin-Embedder-Policy: require-corp
```

## More Info

- Welcome & documentation: https://github.com/samaaron/supersonic/blob/main/docs/WELCOME.md
- Live demo: https://sonic-pi.net/supersonic/demo.html

## Support

Please consider joining the community of supporters enabling Sam's work on creative coding projects:

- Patreon: https://patreon.com/samaaron
- GitHub Sponsors: https://github.com/sponsors/samaaron

## License

GPL v3 - Source code available at https://github.com/samaaron/supersonic

This is a derivative work of [SuperCollider](https://supercollider.github.io/) by James McCartney and the SuperCollider community.
