# Welcome to SuperSonic

>     Greetings friend - I hear you're interested in combining the research-level audio synthesis capabilities of the legendary SuperCollider scsynth engine with the incredible reach of the web?

>     You've come to the right place.

## A Legendary Audio Synthesis in your Browser

SuperCollider's scsynth is a legendary audio synthesis engine designed by James McCartney in the late 90s.

Scsynth has been widely used for decades for audio research, algorithmic composition, live performance systems and instrument augmentation.

SuperSonic brings scsynth into the browser as an AudioWorklet. This enables scsynth to maintain its incredible performance and sub-sample timing accuracy even within the constraints of a web browser.

- **Real-time synthesis** - Create sounds from scratch using oscillators, filters, envelopes, and effects
- **Sample playback** - Load and manipulate audio samples with precise timing control
- **Modular routing** - Connect audio and control signals in any configuration
- **Low latency** - High-performance audio via WebAssembly and AudioWorklet
- **Sample-accurate scheduling** - Precise timing for musical events within the audio engine
- **700+ Unit Generators** - Oscillators, filters, delays, reverbs, granular synthesis, FFT processing, and more
- **OSC control** - Full programmatic control over every parameter in real-time

SuperSonic is perfect for building interactive audio experiences, music tools, live coding environments and experimental sound art.

Happy Coding!

## Quick Start

**Import from CDN** (or self-host)

```javascript
import { SuperSonic } from 'https://unpkg.com/supersonic-scsynth';
```

**Initialise** — zero config needed

```javascript
const supersonic = new SuperSonic();
await supersonic.init();
```

**Load and play a synth**

```javascript
await supersonic.loadSynthDef('sonic-pi-prophet');
supersonic.send('/s_new', 'sonic-pi-prophet', -1, 0, 0, 'note', 60);
```

**Load and play a sample**

```javascript
await supersonic.loadSynthDef('sonic-pi-basic_stereo_player');
await supersonic.loadSample(0, 'bd_haus.flac');
supersonic.send('/s_new', 'sonic-pi-basic_stereo_player', -1, 0, 0, 'buf', 0);
```

## Documentation

- **[API Reference](API.md)** - The SuperSonic JavaScript API for initialising, controlling, and communicating with the scsynth Audioworklet.

## Synthesis

- **[scsynth Command Reference](SCSYNTH_COMMAND_REFERENCE.md)** - The full scsynth OSC command reference for controlling synthesis

## For Contributors

- **[Building](BUILDING.md)** - Building SuperSonic from source
- **[Deployment](DEPLOYMENT.md)** - Deployment guide
- **[Metrics](METRICS.md)** - Performance metrics and monitoring
