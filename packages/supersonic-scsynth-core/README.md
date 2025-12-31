# supersonic-scsynth-core

The SuperCollider scsynth WebAssembly engine and AudioWorklet processor for [SuperSonic](https://github.com/samaaron/supersonic).

## Overview

This package contains the GPL-licensed runtime components:

- `wasm/scsynth-nrt.wasm` - The scsynth engine compiled to WebAssembly
- `workers/*.js` - AudioWorklet processor and supporting workers

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

Then serve the `wasm/` and `workers/` directories from your static file server.

## License

GPL-3.0-or-later

This package is derived from [SuperCollider](https://supercollider.github.io/) by James McCartney and the SuperCollider community.

## Related Packages

- [`supersonic-scsynth`](https://www.npmjs.com/package/supersonic-scsynth) - MIT-licensed client API
- [`supersonic-scsynth-synthdefs`](https://www.npmjs.com/package/supersonic-scsynth-synthdefs) - Synth definitions (MIT)
- [`supersonic-scsynth-samples`](https://www.npmjs.com/package/supersonic-scsynth-samples) - Audio samples (CC0)
- [`supersonic-scsynth-bundle`](https://www.npmjs.com/package/supersonic-scsynth-bundle) - Everything together
