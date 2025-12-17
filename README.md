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

**[Try the live demo](https://sonic-pi.net/supersonic/demo.html)**

## Getting Started

Injecting the full power of SuperCollider's scsynth audio engine into your browser is simple.

Import SuperSonic and initialise it:

```javascript
import { SuperSonic } from "supersonic-scsynth";

const supersonic = new SuperSonic({
  baseURL: "/supersonic/"
});
await supersonic.init();
```

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

Take a look at `example/simple.html` for a minimal working example.

## Installation

Grab the latest pre-built distribution and host it on your server:

```bash
curl -O https://samaaron.github.io/supersonic/supersonic-dist.zip
unzip supersonic-dist.zip
```

Or install via npm:

```bash
npm install supersonic-scsynth-bundle
```

**Note:** SuperSonic must be self-hosted due to browser security requirements around SharedArrayBuffer. It cannot be loaded from a CDN. See [Browser Setup](docs/BROWSER_SETUP.md) for the details.

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
