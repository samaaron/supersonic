# supersonic-scsynth-bundle

Complete SuperSonic bundle with everything included.

## What's Included

This is a convenience meta-package that includes:

- **[supersonic-scsynth](https://www.npmjs.com/package/supersonic-scsynth)** - The core scsynth WASM engine (~450KB)
- **[supersonic-scsynth-synthdefs](https://www.npmjs.com/package/supersonic-scsynth-synthdefs)** - All 120 Sonic Pi synthdefs (~67KB)
- **[supersonic-scsynth-samples](https://www.npmjs.com/package/supersonic-scsynth-samples)** - All 206 Sonic Pi samples (~34MB)

## Installation

```bash
npm install supersonic-scsynth-bundle
```

This installs all three packages as dependencies.

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

| Package | Size | Contains |
|---------|------|----------|
| `supersonic-scsynth` | ~450KB | Core WASM engine |
| `supersonic-scsynth-synthdefs` | ~67KB | 120 Sonic Pi synthdefs |
| `supersonic-scsynth-samples` | ~34MB | 206 Sonic Pi samples |
| `supersonic-scsynth-bundle` | ~2KB | Meta-package (depends on all three) |

## Documentation

See the main [SuperSonic repository](https://github.com/samaaron/supersonic) for full documentation.

## License

GPL-3.0-or-later
