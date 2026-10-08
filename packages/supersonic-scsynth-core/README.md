# supersonic-scsynth-core

The SuperSonic engine for the browser: SuperCollider's scsynth synthesis engine compiled to WebAssembly, running on the clockwork substrate inside an AudioWorklet. It takes OSC in, renders audio out, and answers the scsynth command set. The client that talks to it is [`supersonic-scsynth`](https://www.npmjs.com/package/supersonic-scsynth), which loads this package from wherever you tell it to: a CDN, or a path on your own server.

## Contents

- `wasm/scsynth-nrt.wasm` - the engine
- `wasm/clockwork_midi_bg.wasm`, `wasm/clockwork_gamepad_bg.wasm` - the Web MIDI and gamepad modules, fetched only when the client turns them on (`midi: true`, `gamepad: true`)
- `workers/clockwork_audio_worklet.js` - the AudioWorklet processor that runs the engine on the audio thread
- `index.js` - CDN URLs for this package (`CORE_CDN`, `WASM_CDN`, `WORKLET_CDN`), at `@latest`

## Usage

The client needs to be told where this package is, with `coreBaseURL`. From a CDN:

```javascript
import { SuperSonic } from 'https://unpkg.com/supersonic-scsynth@latest/dist/supersonic.js';

const supersonic = new SuperSonic({
  baseURL: 'https://unpkg.com/supersonic-scsynth@latest/dist/',
  coreBaseURL: 'https://unpkg.com/supersonic-scsynth-core@latest/'
});
```

Without `coreBaseURL` the client looks for the engine under `baseURL`, and the client package ships no wasm, so `init()` fails.

### Self-Hosting

To host the core yourself, point `coreBaseURL` at your copy, alongside the client's own files:

```javascript
import { SuperSonic } from 'supersonic-scsynth';

const supersonic = new SuperSonic({
  workerBaseURL: '/path/to/supersonic-scsynth/dist/workers/',
  coreBaseURL: '/path/to/supersonic-scsynth-core/'
});
```

### Installation

```bash
npm install supersonic-scsynth-core
```

Then serve the `wasm/` directory and `workers/clockwork_audio_worklet.js` from your static file server.

## License

AGPL-3.0-or-later. The engine is scsynth (GPL-3.0-or-later, derived from [SuperCollider](https://supercollider.github.io/) by James McCartney and the SuperCollider community) running on clockwork (AGPL-3.0-or-later), and the combined work is AGPL.

## Related Packages

- [`supersonic-scsynth`](https://www.npmjs.com/package/supersonic-scsynth) - the client API (AGPL-3.0-or-later)
- [`supersonic-scsynth-synthdefs`](https://www.npmjs.com/package/supersonic-scsynth-synthdefs) - synth definitions (MIT)
- [`supersonic-scsynth-samples`](https://www.npmjs.com/package/supersonic-scsynth-samples) - audio samples (CC0)
- [`supersonic-scsynth-bundle`](https://www.npmjs.com/package/supersonic-scsynth-bundle) - everything together
