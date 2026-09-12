# supersonic-scsynth-bundle

Complete SuperSonic bundle with everything included.

## What's Included

This is a convenience meta-package that includes:

- **[supersonic-scsynth](https://www.npmjs.com/package/supersonic-scsynth)** - the client API (AGPL-3.0-or-later)
- **[supersonic-scsynth-core](https://www.npmjs.com/package/supersonic-scsynth-core)** - the WASM engine and its AudioWorklet (AGPL-3.0-or-later)
- **[supersonic-scsynth-synthdefs](https://www.npmjs.com/package/supersonic-scsynth-synthdefs)** - All 119 Sonic Pi synthdefs (~67KB)
- **[supersonic-scsynth-samples](https://www.npmjs.com/package/supersonic-scsynth-samples)** - All 207 Sonic Pi samples (~34MB)

## Installation

```bash
npm install supersonic-scsynth-bundle
```

This installs all four packages as dependencies.

## Usage

```javascript
import { SuperSonic } from 'https://unpkg.com/supersonic-scsynth@latest';

const supersonic = new SuperSonic();
await supersonic.init();
await supersonic.loadSynthDefs(['sonic-pi-beep', 'sonic-pi-tb303']);
```

SuperSonic works directly from CDN with zero configuration using the default `postMessage` mode. For lower latency, use `mode: 'sab'` which requires COOP/COEP headers.

## When to Use This

**Use this bundle if:**
- You want a quick start with everything included
- You want access to all the Sonic Pi synths and samples

**Use separate packages if:**
- You want minimal package size (just install `supersonic-scsynth`)
- You have your own synthdefs and samples
- You're building for production and want fine-grained control

## Package Breakdown

| Package | Size | License | Contains |
|---------|------|---------|----------|
| `supersonic-scsynth` | ~140KB (~40KB gzipped) | AGPL-3.0-or-later | Client API, OSC workers, metrics component |
| `supersonic-scsynth-core` | ~1.9MB | AGPL-3.0-or-later | WASM engine + AudioWorklet |
| `supersonic-scsynth-synthdefs` | ~67KB | MIT | 119 Sonic Pi synthdefs |
| `supersonic-scsynth-samples` | ~34MB | CC0 | 207 Sonic Pi samples |
| `supersonic-scsynth-bundle` | ~2KB | Mixed | Meta-package (depends on all four) |

## Documentation

See the main [SuperSonic repository](https://github.com/samaaron/supersonic) for full documentation.

## License

Mixed - see individual packages:
- `supersonic-scsynth` and `supersonic-scsynth-core` - AGPL-3.0-or-later (scsynth on clockwork)
- `supersonic-scsynth-synthdefs` - MIT
- `supersonic-scsynth-samples` - CC0
