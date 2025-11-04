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

Same API as using the packages separately:

```javascript
import { SuperSonic } from 'supersonic-scsynth-bundle';

const sonic = new SuperSonic({
  audioBaseURL: 'https://unpkg.com/supersonic-scsynth-samples@latest/samples/'
});

await sonic.init();
await sonic.loadSynthDefs(
  ['sonic-pi-beep', 'sonic-pi-tb303'],
  'https://unpkg.com/supersonic-scsynth-synthdefs@latest/synthdefs/'
);
```

## When to Use This

**Use this bundle if:**
- You want a quick start with everything included
- You're building a Sonic Pi-compatible application
- You want the Sonic Pi synthdefs and samples

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
