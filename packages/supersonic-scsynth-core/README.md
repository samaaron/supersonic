# supersonic-scsynth-core

The SuperSonic engine for the browser: SuperCollider's scsynth synthesis engine compiled to WebAssembly, running on the clockwork substrate inside an AudioWorklet. It takes OSC in, renders audio out, and answers the scsynth command set. The client that talks to it is [`supersonic-scsynth`](https://www.npmjs.com/package/supersonic-scsynth), which loads this package from a CDN or from a path you give it.

## Contents

- `wasm/scsynth-nrt.wasm` - the engine
- `workers/clockwork_audio_worklet.js` - the AudioWorklet processor that runs it on the audio thread

## Usage

This package is typically loaded automatically by `supersonic-scsynth` from CDN:

```javascript
import { SuperSonic } from 'supersonic-scsynth';

const supersonic = new SuperSonic();
// Loads core from: https://unpkg.com/supersonic-scsynth-core@latest/
```

### Self-Hosting

To host the core yourself:

```javascript
import { SuperSonic } from 'supersonic-scsynth';

const supersonic = new SuperSonic({
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
