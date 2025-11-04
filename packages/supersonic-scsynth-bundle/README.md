# supersonic-scsynth-bundle

Complete SuperSonic bundle with everything included.

## What's Included

This is a convenience meta-package that includes:

- **[supersonic-scsynth](https://www.npmjs.com/package/supersonic-scsynth)** - The core scsynth WASM engine (~2MB)
- **[supersonic-scsynth-extra](https://www.npmjs.com/package/supersonic-scsynth-extra)** - Sonic Pi synthdefs and samples (~8MB)

## Installation

```bash
npm install supersonic-scsynth-bundle
```

This installs both packages as dependencies.

## Usage

Same API as using the packages separately:

```javascript
import { SuperSonic, SAMPLES_CDN, SYNTHDEFS_CDN } from 'supersonic-scsynth-bundle';

const sonic = new SuperSonic({
  audioBaseURL: SAMPLES_CDN
});

await sonic.init();
await sonic.loadSynthDefs(['sonic-pi-beep', 'sonic-pi-tb303'], SYNTHDEFS_CDN);
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
| `supersonic-scsynth` | ~2MB | Core engine only |
| `supersonic-scsynth-extra` | ~8MB | Synthdefs + samples |
| `supersonic-scsynth-bundle` | ~10KB | Meta-package (depends on both) |

## Documentation

See the main [SuperSonic repository](https://github.com/samaaron/supersonic) for full documentation.

## License

GPL-3.0-or-later
