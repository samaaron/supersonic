> **Note: Alpha Status**: SuperSonic is in active development. The API may evolve, but the core synthesis engine is solid and ready for experimentation. Feedback and ideas are most welcome.

```
░█▀▀░█░█░█▀█░█▀▀░█▀▄░█▀▀░█▀█░█▀█░▀█▀░█▀▀
░▀▀█░█░█░█▀▀░█▀▀░█▀▄░▀▀█░█░█░█░█░░█░░█░░
░▀▀▀░▀▀▀░▀░░░▀▀▀░▀░▀░▀▀▀░▀▀▀░▀░▀░▀▀▀░▀▀▀
```

**SuperSonic** - [SuperCollider](https://supercollider.github.io/)'s powerful audio synthesis engine scsynth running in the browser as an [AudioWorklet](https://developer.mozilla.org/en-US/docs/Web/API/AudioWorklet).

- _AudioWorklet_ - runs in a dedicated high priority audio thread
- _WebAssembly_ - scsynth's C++ code compiled for the web
- _OSC API_ - talk to the scsynth server through its native OSC API
- _Zero Config CDN_ - works directly from unpkg with no server setup

**[Try the live demo](https://sonic-pi.net/supersonic/demo.html)**

## Getting Started

### CDN (Zero Config)

The simplest way to use SuperSonic - no server setup required:

```html
<script type="module">
  import { SuperSonic } from "https://unpkg.com/supersonic-scsynth@latest";

  const supersonic = new SuperSonic();
  await supersonic.init();
  await supersonic.loadSynthDef("sonic-pi-prophet");
  supersonic.send("/s_new", "sonic-pi-prophet", -1, 0, 0, "note", 60);
</script>
```

All URLs are auto-detected from the import path. See `example/cdn.html` for a working example.

### Self-Hosted

You can also host the files yourself:

```javascript
import { SuperSonic } from "./supersonic/supersonic.js";

const supersonic = new SuperSonic();  // URLs auto-detected from import path
await supersonic.init();
```

Or with explicit configuration:

```javascript
const supersonic = new SuperSonic({
  baseURL: "/supersonic/"
});
await supersonic.init();
```

### Playing Sounds

Load and play a synth:

```javascript
await supersonic.loadSynthDef("sonic-pi-prophet");
supersonic.send("/s_new", "sonic-pi-prophet", -1, 0, 0, "note", 60);
```

Load and play a sample:

```javascript
await supersonic.loadSynthDef("sonic-pi-basic_stereo_player");
await supersonic.loadSample(0, "loop_amen.flac");
supersonic.send("/s_new", "sonic-pi-basic_stereo_player", -1, 0, 0, "buf", 0);
```

See `example/simple.html` for a minimal working example.

## Installation

### Option 1: CDN (Recommended for Getting Started)

No installation needed - just import directly:

```javascript
import { SuperSonic } from "https://unpkg.com/supersonic-scsynth@latest";
```

### Option 2: npm

```bash
npm install supersonic-scsynth
```

### Option 3: Self-Hosted Bundle

Download the pre-built distribution from [GitHub Releases](https://github.com/samaaron/supersonic/releases):

```bash
curl -LO https://github.com/samaaron/supersonic/releases/latest/download/supersonic.zip
unzip supersonic.zip
```

This gives you:

```
supersonic/
├── supersonic.js      # Main library
├── wasm/              # WebAssembly binaries
├── workers/           # Web Workers
├── synthdefs/         # 127 synth definitions
└── samples/           # 206 audio samples
```

## Documentation

- [API Reference](docs/API.md) - Methods, callbacks, and configuration
- [Server Command Reference](docs/SERVER_COMMAND_REFERENCE.md) - OSC commands for controlling scsynth
- [Metrics](docs/METRICS.md) - Performance monitoring and debugging
- [Browser Setup](docs/BROWSER_SETUP.md) - Required headers and browser requirements
- [CDN and Self-Hosting](docs/CDN.md) - Why self-hosting is required
- [Building from Source](docs/BUILDING.md) - Compiling the WASM yourself

## Support

SuperSonic is brought to you by Sam Aaron. Please consider joining the community of supporters enabling Sam's work on creative coding projects like this, [Sonic Pi](https://sonic-pi.net) and [Tau5](https://tau5.live).

- [Patreon](https://patreon.com/samaaron)
- [GitHub Sponsors](https://github.com/sponsors/samaaron)

## License

GPL v3 - This is a derivative work of SuperCollider

## Credits

Based on [SuperCollider](https://supercollider.github.io/) by James McCartney and the SuperCollider community. This AudioWorklet port was inspired by Hanns Holger Rutz who started the first port of scsynth to WASM and Dennis Scheiba who continued this work. Thank you to everyone in the SuperCollider community!
