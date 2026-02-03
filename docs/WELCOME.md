
```
░█▀▀░█░█░█▀█░█▀▀░█▀▄░█▀▀░█▀█░█▀█░▀█▀░█▀▀
░▀▀█░█░█░█▀▀░█▀▀░█▀▄░▀▀█░█░█░█░█░░█░░█░░
░▀▀▀░▀▀▀░▀░░░▀▀▀░▀░▀░▀▀▀░▀▀▀░▀░▀░▀▀▀░▀▀▀
```
>     Greetings friend,
>
>     I hear you're interested in combining the research-level
>     audio synthesis capabilities of the legendary SuperCollider
>     scsynth engine with the incredible reach of the web?

>     Welcome, you've come to the right place.

# A Legendary Audio Synth in your Browser

SuperCollider's scsynth is a legendary audio synthesis engine designed by James McCartney in the late 90s.

__Scsynth has been widely used for decades for audio research, algorithmic composition, live performance systems and instrument augmentation.__

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

## Hello World

```javascript
import { SuperSonic } from 'https://unpkg.com/supersonic-scsynth@latest';

const supersonic = new SuperSonic({
  baseURL: 'https://unpkg.com/supersonic-scsynth@latest/dist/',
  synthdefBaseURL: 'https://unpkg.com/supersonic-scsynth-synthdefs@latest/synthdefs/',
});
await supersonic.init();

await supersonic.loadSynthDef('sonic-pi-prophet');
supersonic.send('/s_new', 'sonic-pi-prophet', -1, 0, 0, 'note', 60);
```

For other installation options including npm and self-hosting, see the [Installation Guide](INSTALLATION.md).

## Documentation

- **[Installation](INSTALLATION.md)** - CDN, npm, self-hosting options
- **[Quick Start](QUICKSTART.md)** - Boot and play your first synth
- **[API Reference](API.md)** - The SuperSonic JavaScript API for initialising, controlling, and communicating with the scsynth Audioworklet
- **[Communication Modes](MODES.md)** - SAB vs postMessage, server configuration

## Synthesis

- **[scsynth Command Reference](SCSYNTH_COMMAND_REFERENCE.md)** - The full scsynth OSC command reference for controlling synthesis

## For Contributors

- **[Building](BUILDING.md)** - Building SuperSonic from source
- **[Metrics](METRICS.md)** - Performance metrics and monitoring
