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

SuperSonic supports two transport modes: **postMessage** (default, works everywhere) and **SAB** (SharedArrayBuffer, lower latency but requires COOP/COEP server headers).

For SAB mode, set `mode: 'sab'` and configure your server headers. See the full documentation for details.

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
