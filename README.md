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

**SuperSonic** is a complete reworking of [SuperCollider](https://supercollider.github.io/)'s audio synthesis engine **scsynth** designed to run in new places - in the browser as an [AudioWorklet](https://developer.mozilla.org/en-US/docs/Web/API/AudioWorklet), as a standalone native server, or as a native shared library running directly in the [BEAM](https://www.erlang.org/) via a NIF. Currently it has the following features:

### Built on clockwork

SuperSonic is scsynth running on [clockwork](https://github.com/samaaron/clockwork). clockwork provides the audio device IO, MIDI, OSC, gamepad input, the transport clock and, natively, Ableton Link; scsynth provides the synthesis engine, node tree and UGens. The two connect through Clockwork's DSP API in `dsp_api.h`.

### Core

- **scsynth compatible** - *full OSC command compatibility with SuperCollider's scsynth. See the [command reference](docs/SCSYNTH_COMMAND_REFERENCE.md).*
- **Live metrics** - *cheap, zero-copy telemetry in both transport modes: OSC throughput, scheduler depth and lateness, ring buffer fill and a live node-tree mirror. See [metrics](docs/METRICS.md).*
- **Malloc-free audio path** - *zero allocation or blocking on the audio thread.*
- **Scheduling in the engine** - *far-future bundles are held by the engine itself, timestamped to the sample; `/clearSched` cancels what is still waiting.*
- **Cold swap** - *the engine can be rebuilt under a running client, for a device or rate change, with synthdefs and buffers restored by the client.*
- **Upstream compatible** - *kept in sync with the official SuperCollider scsynth server.*

### Web

- **Dual transport** - *SharedArrayBuffer for performance, postMessage for zero-config CDN deployment.*
- **Mobile resilient** - *suspend and resume, a full reload when resume fails, and the client restores its synthdefs and buffers afterwards.*
- **Observable** - *real-time telemetry: ring buffer usage, scheduler depth, audio health and glitch detection.*
- **Multiple clients** - *give any Web Worker its own OscChannel to the AudioWorklet. Each channel carries a source ID visible in the aggregated OSC log.*
- **Thread-safe node IDs** - *`nextNodeId()` allocates unique node IDs across any number of threads and workers - guaranteed unique with no clashes.*
- **Hosted on npm** - *available as `supersonic-scsynth` with separate packages for core, synthdefs and samples.*

### Native

- **Ableton Link v4** - *tempo sync across the network plus streaming Link Audio in and out.*
- **Live device and driver switching** - *hot-swap at runtime with automatic cold swap on rate mismatch.*
- **Headless mode** - *high-resolution timer-driven processing for CI and containers.*
- **scsynth's command line and OSC** - *a drop-in scsynth replacement over UDP, or TCP, Unix sockets, a named pipe or shared memory, with clockwork's `/clockwork/*` verbs for devices, MIDI, gamepad and the clock alongside. See the [native guide](docs/NATIVE.md).*
- **Plugin hosting** - *tracks of CLAP and VST3 plugins, run in a separate bridge process.*
- **Session recording** - *the main output to WAV or FLAC.*

### NIF

- **BEAM embedded** - *clean OSC binary interface. Same protocol boundary as web and native.*
- **Never blocks a scheduler** - *`send_osc` is a ring write; `start` and `stop` hand the slow boot and shutdown to the NIF's own thread and report back by message.*
- **PID-based notifications** - *any number of processes register, and each receives every OSC reply and debug line as a message.*

## Demo

Try the live demo: [**sonic-pi.net/supersonic/demo.html**](https://sonic-pi.net/supersonic/demo.html)

## Getting Started

### Web

SuperSonic can be fetched remotely via CDN, locally via npm or self-built — see [Installation](docs/INSTALLATION_WEB.md). Once installed, head to the [Quick Start](docs/QUICKSTART.md) to make your first sound; every configuration option is in the [API Reference](docs/API.md#constructor-options).

### Native

Clone with the submodule and build with CMake - see [Building from Source](docs/BUILDING.md).

    git clone --recurse-submodules https://github.com/samaaron/supersonic
    cmake -B build/native -DCMAKE_BUILD_TYPE=Release
    cmake --build build/native --target SuperSonic

`CMakeLists.txt` here names the guest and links the host; everything else is clockwork's own build - see [clockwork/docs/BUILDING.md](clockwork/docs/BUILDING.md) for the options. The [native guide](docs/NATIVE.md) covers running it: options, devices, transports and logs. On Debian, [Debian Packaging](docs/DEBIAN-PACKAGING.md) builds a package from source.

### NIF

Build the BEAM NIF - no Erlang installation is needed to build it:

    cmake -B build/nif -DCLOCKWORK_NIF=ON -DCMAKE_BUILD_TYPE=Release
    cmake --build build/nif --target clockwork_nif --config Release --parallel

The library lands at `build/nif/clockwork.so` (`clockwork.dll` on Windows). See the [NIF guide](docs/NIF.md) to load and drive it from Elixir or Erlang.

## Documentation

### Every platform

- [scsynth Command Reference](docs/SCSYNTH_COMMAND_REFERENCE.md) - scsynth's OSC commands, and what each platform answers
- [Differences from scsynth](docs/SCSYNTH_DIFFERENCES.md) - what SuperSonic does differently, platform by platform
- [OSC API](docs/OSC_API.md) - clockwork's `/clockwork/*` verbs: devices, clock and Link, MIDI, recording, tracks
- [Metrics](docs/METRICS.md) - performance monitoring and debugging
- [Building from Source](docs/BUILDING.md) - web, native and NIF builds

### Web

- [Installation](docs/INSTALLATION_WEB.md) - CDN, npm, self-hosting, browser requirements
- [Quick Start](docs/QUICKSTART.md) - boot and play your first synth
- [API Reference](docs/API.md) - methods, events and configuration
- [Guide](docs/GUIDE.md) - OSC encoding, lifecycle and recovery, audio routing, samples
- [Communication Modes](docs/MODES.md) - SAB and postMessage
- [Workers Guide](docs/WORKERS.md) - send OSC directly from Web Workers and AudioWorklets
- [Architecture](docs/ARCHITECTURE.md) - how the web host fits together

### Native

- [Native Guide](docs/NATIVE.md) - running the server: options, devices, transports, logs
- [Debian Packaging](docs/DEBIAN-PACKAGING.md) - the Debian source package: offline build, system dependencies, lintian/autopkgtest

### NIF

- [NIF Guide](docs/NIF.md) - load and drive the engine from Erlang or Elixir

## Support

SuperSonic is brought to you by Sam Aaron. Please consider joining the community of supporters enabling Sam's work on creative coding projects like this, [Sonic Pi](https://sonic-pi.net) and [Tau5](https://tau5.live).

- [Patreon](https://patreon.com/samaaron)
- [GitHub Sponsors](https://github.com/sponsors/samaaron)

## License

SuperSonic as a whole is copyleft: scsynth is GPL-3.0-or-later and clockwork is AGPL-3.0-or-later (or commercially licensed), so the combined program is AGPL-3.0-or-later. The clockwork half keeps its own licence and stays separable. See [LICENSE](LICENSE) and [clockwork/LICENSE](clockwork/LICENSE).

## Credits

The synth is based on [SuperCollider](https://supercollider.github.io/) by James McCartney and the SuperCollider community. This AudioWorklet port was inspired by Hanns Holger Rutz who started the first port of scsynth to WASM and Dennis Scheiba who continued this work. Thank you to everyone in the SuperCollider community!
