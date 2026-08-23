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

This is SuperSonic. All the synthesis power of the original **scsynth** - rearchitected to reach new places.

# Welcome to SuperSonic

**SuperSonic** is a complete reworking of [SuperCollider](https://supercollider.github.io/)'s audio synthesis engine **scsynth** designed to run in new places - in the browser as an [AudioWorklet](https://developer.mozilla.org/en-US/docs/Web/API/AudioWorklet), as a standalone native server using [JUCE](https://juce.com), on embedded hardware such as the ESP32-S3, or as a native shared library running directly in the [BEAM](https://www.erlang.org/) via a NIF. Currently it has the following features:

### Core

- **scsynth compatible** - *full OSC command compatibility with SuperCollider's scsynth. See the [command reference](docs/SCSYNTH_COMMAND_REFERENCE.md).*
- **Live metrics** - *cheap, zero-copy telemetry in both transport modes: OSC throughput, scheduler depth and lateness, ring buffer fill and a live node-tree mirror. See [metrics](docs/METRICS.md).*
- **Malloc-free audio path** - *zero allocation or blocking on the audio thread.*
- **Pre-scheduler** - *dynamically growing holding bay for future OSC bundles with cancellation support.*
- **Cold-swap recovery** - *multiple restart levels with automatic synthdef and buffer restoration.*
- **Upstream compatible** - *kept in sync with the official SuperCollider scsynth server.*
- **Tested** - *1400+ tests across web, native and NIF targets.*

### Web

- **Dual transport** - *SharedArrayBuffer for performance, postMessage for zero-config CDN deployment.*
- **Mobile resilient** - *suspend, resume and worklet death detection with automatic state restoration.*
- **Observable** - *real-time telemetry: ring buffer usage, scheduler depth, audio health and glitch detection.*
- **Multiple clients** - *give any Web Worker its own OscChannel to the AudioWorklet with automatic pre-scheduler routing for far-future events. Each channel carries a source ID visible in the aggregated OSC log.*
- **Thread-safe node IDs** - *`nextNodeId()` allocates unique node IDs across any number of threads and workers - guaranteed unique with no clashes.*
- **Hosted on npm** - *available as `supersonic-scsynth` with separate packages for core, synthdefs and samples.*

### Native

- **Ableton Link v4** - *tempo sync across the network plus streaming Link Audio in and out.*
- **Live device and driver switching** - *hot-swap at runtime with automatic cold swap on rate mismatch.*
- **Headless mode** - *high-resolution timer-driven processing for CI and containers.*
- **UDP OSC server** - *drop-in scsynth replacement with `/supersonic/*` device and recording commands.*

### Embedded

- **Two-tiered memory** - *a placement allocator routes audio-hot allocations to fast internal SRAM and bulk data to external PSRAM with graceful spillover; a zero-overhead passthrough on single-region platforms.*
- **ESP32-S3** - *memory profile and tiered allocator in place for the ESP32-S3R8 (512 KiB SRAM + 8 MiB PSRAM); on-device build and tuning still in progress.*

### NIF

- **BEAM embedded** - *clean OSC binary interface. Same protocol boundary as web and native.*
- **Dirty scheduler aware** - *engine init never blocks normal BEAM schedulers.*
- **PID-based notifications** - *receive OSC replies and debug output asynchronously via `enif_send()`.*

## Demo

Try the live demo: [**sonic-pi.net/supersonic/demo.html**](https://sonic-pi.net/supersonic/demo.html)

## Getting Started

### Web

SuperSonic can be fetched remotely via CDN, locally via npm or self-built — see [Installation](docs/INSTALLATION_WEB.md).

### Native

Build the standalone backend with `scripts/build-native.sh` — see [Building from Source](docs/BUILDING.md).

### NIF

Build the BEAM NIF with `scripts/build-nif.sh` — see [Building from Source](docs/BUILDING.md).

For the full list of configuration options, see the [API Reference](docs/API.md#constructor-options). For installation options see the [Installation Guide](docs/INSTALLATION_WEB.md). Once installed, head to the [Quick Start](docs/QUICKSTART.md) to make your first sound.


## Documentation

- [Installation](docs/INSTALLATION_WEB.md) - CDN, npm, self-hosting, browser requirements
- [Quick Start](docs/QUICKSTART.md) - Boot and play your first synth
- [API Reference](docs/API.md) - Methods, callbacks, and configuration
- [Communication Modes](docs/MODES.md) - SAB vs postMessage, server configuration
- [scsynth Command Reference](docs/SCSYNTH_COMMAND_REFERENCE.md) - OSC commands for controlling scsynth
- [Workers Guide](docs/WORKERS.md) - Send OSC directly from Web Workers and AudioWorklets for the lowest latency.
- [Metrics](docs/METRICS.md) - Performance monitoring and debugging
- [Building from Source](docs/BUILDING.md) - WASM, native (JUCE), and NIF (Erlang/Elixir) builds
- [Debian Packaging](docs/DEBIAN-PACKAGING.md) - CI-proven Debian source package: offline build, system dependencies, lintian/autopkgtest

## Support

SuperSonic is brought to you by Sam Aaron. Please consider joining the community of supporters enabling Sam's work on creative coding projects like this, [Sonic Pi](https://sonic-pi.net) and [Tau5](https://tau5.live).

- [Patreon](https://patreon.com/samaaron)
- [GitHub Sponsors](https://github.com/sponsors/samaaron)

## License

See [LICENSE](LICENSE) for details.         

## Credits

The synth is based on [SuperCollider](https://supercollider.github.io/) by James McCartney and the SuperCollider community. This AudioWorklet port was inspired by Hanns Holger Rutz who started the first port of scsynth to WASM and Dennis Scheiba who continued this work. Thank you to everyone in the SuperCollider community!
