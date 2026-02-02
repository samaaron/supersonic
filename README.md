> **Note: Still Alpha Status**: _SuperSonic is in active development._ The API may continue to evolve, but the core synthesis engine is solid and ready for experimentation. _Feedback and ideas are most welcome._

```
░█▀▀░█░█░█▀█░█▀▀░█▀▄░█▀▀░█▀█░█▀█░▀█▀░█▀▀
░▀▀█░█░█░█▀▀░█▀▀░█▀▄░▀▀█░█░█░█░█░░█░░█░░
░▀▀▀░▀▀▀░▀░░░▀▀▀░▀░▀░▀▀▀░▀▀▀░▀░▀░▀▀▀░▀▀▀
```

Back in the late 90s James McCartney designed a series of live audio programming environments called [SuperCollider](https://en.wikipedia.org/wiki/SuperCollider). These were systems with both programming languages and audio runtimes carefully designed for live realtime modification at every level - from high sweeping programming language abstractions all the way down to the fine control of the low-level synthesis components of the audio chain.

One of the many gifts from this work is **scsynth** - the core synthesis engine James created for version 3 of SuperCollider. It was at this point when he _formally separated the language from the synth engine_.

This split made it possible to combine **scsynth**'s powerful audio synthesis capabilities with any existing - or yet to exist - programming language.

This then led to a suite of powerful new live coding languages using **scsynth** for audio synthesis.

_What if you didn't just bring your language to scsynth? What if you brought scsynth to your environment?_

This is SuperSonic. All the synthesis power of **scsynth** - modified and augmented to run in your web browser.

# Welcome to SuperSonic

**SuperSonic** is [SuperCollider](https://supercollider.github.io/)'s powerful audio synthesis engine **scsynth** running in the browser as an [AudioWorklet](https://developer.mozilla.org/en-US/docs/Web/API/AudioWorklet).

Highlights:

- **AudioWorklet** - runs in a dedicated high priority audio thread
- **WebAssembly** - scsynth's original C++ code compiled for the web
- **OSC API** - talk to the scsynth server through its native OSC API
- **Zero Config via CDN** - no installation necessary - works directly from CDNs such as unpkg.
- **Optional SAB mode** - can use a SharedArrayBuffer (SAB) for lower latency and reduced jitter with internal comms. Requires COOP/COEP headers to enable browsers to use the SAB


## Demo

Try the live demo: [**sonic-pi.net/supersonic/demo.html**](https://sonic-pi.net/supersonic/demo.html)


## Getting Started

### CDN Usage

```html
<script type="module">
  import { SuperSonic } from 'https://unpkg.com/supersonic-scsynth@latest';

  const sonic = new SuperSonic({
    baseURL: 'https://unpkg.com/supersonic-scsynth@latest/dist/',
    sampleBaseURL: 'https://unpkg.com/supersonic-scsynth-samples@latest/samples/',
    synthdefBaseURL: 'https://unpkg.com/supersonic-scsynth-synthdefs@latest/synthdefs/',
  });
  await sonic.init();
</script>
```

### npm / Bundler Usage

```javascript
import { SuperSonic } from 'supersonic-scsynth';

const sonic = new SuperSonic({
  baseURL: '/assets/supersonic/'  // Where you serve the WASM/workers
});
```

### Configuration Options

| Option | Description |
|--------|-------------|
| `baseURL` | Base path for all assets (convenience shorthand) |
| `coreBaseURL` | Path to WASM and workers (overrides baseURL) |
| `wasmUrl` | Direct URL to scsynth-nrt.wasm |
| `workletUrl` | Direct URL to audio worklet JS |
| `sampleBaseURL` | Path to sample files |
| `synthdefBaseURL` | Path to synthdef files |
| `mode` | `'sab'` or `'postMessage'` - transport mode (defaults to `'postMessage'`) - see below |

For the full list of configuration options, see the [API Reference](docs/API.md#constructor-options). For installation options see the [Installation Guide](docs/INSTALLATION.md). Once installed, head to the [Quick Start](docs/QUICKSTART.md) to make your first sound.


### Communication Modes

SuperSonic has two communication modes:

- **SAB mode** (SharedArrayBuffer) - the main thread, each worker and audio worklet (i.e. isolated thread) has access to the same memory section for reading and writing. This lowers communication latency and jitter but requires your server to send [COOP/COEP headers](https://web.dev/articles/coop-coep).
- **PM mode** (postMessage) - the main thread, each worker and audio worklet communicate via explicit messages using `postMessage`. This mode does not require special HTTP headers and works anywhere including CDNs. Due to the extra work of sending explicit messages between workers and the audio worklet, this mode has slightly higher internal latency and jitter.

SuperSonic defaults to `'postMessage'` mode which works everywhere including CDNs. If you're self-hosting, running something in production and want the lowest latency and highest robustness, set `mode: 'sab'` - but note that if the required COOP/COEP headers are not present, `init()` will throw an error.

Set the mode in the SuperSonic constructor with `mode: 'postMessage'` or `mode: 'sab'` (See above).


## Documentation

- [Installation](docs/INSTALLATION.md) - CDN, npm, self-hosting, browser requirements
- [Quick Start](docs/QUICKSTART.md) - Boot and play your first synth
- [API Reference](docs/API.md) - Methods, callbacks, and configuration
- [scsynth Command Reference](docs/SCSYNTH_COMMAND_REFERENCE.md) - OSC commands for controlling scsynth
- [Workers Guide](docs/WORKERS.md) - Sending OSC from Web Workers
- [Metrics](docs/METRICS.md) - Performance monitoring and debugging
- [Building from Source](docs/BUILDING.md) - Compiling the WASM yourself


## Support

SuperSonic is brought to you by Sam Aaron. Please consider joining the community of supporters enabling Sam's work on creative coding projects like this, [Sonic Pi](https://sonic-pi.net) and [Tau5](https://tau5.live).

- [Patreon](https://patreon.com/samaaron)
- [GitHub Sponsors](https://github.com/sponsors/samaaron)


## License

SuperSonic uses a tiered licensing model - see [LICENSE](LICENSE) for details.

| Package | License | Contains |
|---------|---------|----------|
| `supersonic-scsynth` | MIT | Client API |
| `supersonic-scsynth-core` | GPL-3.0-or-later | WASM engine + workers |
| `supersonic-scsynth-synthdefs` | MIT | Synth definitions |
| `supersonic-scsynth-samples` | CC0 | Audio samples |


## Credits

Based on [SuperCollider](https://supercollider.github.io/) by James McCartney and the SuperCollider community. This AudioWorklet port was inspired by Hanns Holger Rutz who started the first port of scsynth to WASM and Dennis Scheiba who continued this work. Thank you to everyone in the SuperCollider community!
