# SuperSonic {{VERSION}}

SuperCollider's powerful **scsynth** audio synthesis engine running in the browser as an AudioWorklet.

## Quick Start

Serve the directory that holds this `supersonic/` folder from a web server (over HTTPS, or from `localhost`), and put a page there:

```html
<button>play</button>
```

```javascript
import { SuperSonic } from './supersonic/supersonic.js';

const supersonic = new SuperSonic({
  baseURL: './supersonic/'
});

// Browsers start audio only after a click, tap or keypress
document.querySelector('button').onclick = async () => {
  await supersonic.init();
  await supersonic.loadSynthDef('sonic-pi-beep');
  supersonic.send('/s_new', 'sonic-pi-beep', -1, 0, 0, 'note', 60);
};
```

## Transport Modes

SuperSonic has two transport modes: **SAB** (SharedArrayBuffer, lower latency) and **postMessage** (works everywhere). It uses SAB when the page is cross-origin isolated - its server sends the COOP/COEP headers - and postMessage when it is not. Set `mode: 'sab'` or `mode: 'postMessage'` to choose. See the full documentation for details.

## More Info

- Welcome & documentation: https://github.com/samaaron/supersonic/blob/main/docs/WELCOME.md
- Live demo: https://sonic-pi.net/supersonic/demo.html

## Support

Please consider joining the community of supporters enabling Sam's work on creative coding projects:

- Patreon: https://patreon.com/samaaron
- GitHub Sponsors: https://github.com/sponsors/samaaron

## License

AGPL-3.0-or-later - Source code available at https://github.com/samaaron/supersonic

The synthdefs in `synthdefs/` are MIT, and the samples in `samples/` are CC0.

This is a derivative work of [SuperCollider](https://supercollider.github.io/) by James McCartney and the SuperCollider community.
