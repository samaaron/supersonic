# SuperSonic Documentation

Welcome to SuperSonic

Greetings friend - I hear you're interested in combining the research-level audio synthesis capabilities of the legendary SuperCollider scsynth engine with the incredible reach of the web?

You've come to the right place.

## Legendary Audio Synthesis in your Browser

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

SuperSonic is perfect for building interactive audio experiences, music tools, live coding environments and experimental sound art. Happy Coding!

## Getting Started

- **[API Reference](API.md)** - The SuperSonic JavaScript API for initialising, controlling, and communicating with scsynth.

## Synthesis

- **[scsynth Command Reference](SCSYNTH_COMMAND_REFERENCE.md)** - The full scsynth OSC command reference for controlling synthesis
- **[Loading SynthDefs](SYNTHDEF_LOADING.md)** - How to load and use synth definitions

## For Contributors

- **[Building](BUILDING.md)** - Building SuperSonic from source
- **[Deployment](DEPLOYMENT.md)** - Deployment guide
- **[Metrics](METRICS.md)** - Performance metrics and monitoring
- **[Upstream Sync Guide](UPSTREAM_SYNC_GUIDE.md)** - Keeping in sync with upstream SuperCollider
