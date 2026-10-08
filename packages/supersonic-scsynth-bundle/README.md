# supersonic-scsynth-bundle

Complete SuperSonic bundle with everything included.

## What's Included

This is a convenience meta-package that includes:

- **[supersonic-scsynth](https://www.npmjs.com/package/supersonic-scsynth)** - the client API (AGPL-3.0-or-later)
- **[supersonic-scsynth-core](https://www.npmjs.com/package/supersonic-scsynth-core)** - the WASM engine and its AudioWorklet (AGPL-3.0-or-later)
- **[supersonic-scsynth-synthdefs](https://www.npmjs.com/package/supersonic-scsynth-synthdefs)** - Sonic Pi's synthdefs
- **[supersonic-scsynth-samples](https://www.npmjs.com/package/supersonic-scsynth-samples)** - Sonic Pi's samples (about 35 MB)

## Installation

```bash
npm install supersonic-scsynth-bundle
```

This installs all four packages as dependencies.

## Usage

Tell the client where each package is. From a CDN:

```javascript
import { SuperSonic } from 'https://unpkg.com/supersonic-scsynth@latest/dist/supersonic.js';

const supersonic = new SuperSonic({
  baseURL: 'https://unpkg.com/supersonic-scsynth@latest/dist/',
  coreBaseURL: 'https://unpkg.com/supersonic-scsynth-core@latest/',
  synthdefBaseURL: 'https://unpkg.com/supersonic-scsynth-synthdefs@latest/synthdefs/',
  sampleBaseURL: 'https://unpkg.com/supersonic-scsynth-samples@latest/samples/'
});

// Browsers start audio only after a click, tap or keypress
document.querySelector('button').onclick = async () => {
  await supersonic.init();
  await supersonic.loadSynthDefs(['sonic-pi-beep', 'sonic-pi-tb303']);
};
```

From your own server, serve the installed packages' files and point the same options at them. SuperSonic uses the SAB transport, which has lower latency, when your page is cross-origin isolated (served with COOP/COEP headers), and postMessage when it is not.

## When to Use This

**Use this bundle if:**
- You want a quick start with everything included
- You want access to the Sonic Pi synths and samples

**Use separate packages if:**
- You want minimal package size (just install `supersonic-scsynth` and `supersonic-scsynth-core`)
- You have your own synthdefs and samples
- You're building for production and want fine-grained control

## Package Breakdown

| Package | License | Contains |
|---------|---------|----------|
| `supersonic-scsynth` | AGPL-3.0-or-later | Client API, OSC workers, metrics component |
| `supersonic-scsynth-core` | AGPL-3.0-or-later | WASM engine + AudioWorklet, Web MIDI and gamepad modules |
| `supersonic-scsynth-synthdefs` | MIT | Sonic Pi synthdefs |
| `supersonic-scsynth-samples` | CC0 | Sonic Pi samples |
| `supersonic-scsynth-bundle` | Mixed | Meta-package (depends on all four) |

## Documentation

See the main [SuperSonic repository](https://github.com/samaaron/supersonic) for full documentation.

## License

Mixed - see individual packages:
- `supersonic-scsynth` and `supersonic-scsynth-core` - AGPL-3.0-or-later (scsynth on clockwork)
- `supersonic-scsynth-synthdefs` - MIT
- `supersonic-scsynth-samples` - CC0
