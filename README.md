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

This is SuperSonic. All the synthesis power of **scsynth** - rearchitected to reach new places.

# Welcome to SuperSonic

**SuperSonic** is a reworking of [SuperCollider](https://supercollider.github.io/)'s audio synthesis engine **scsynth** designed to run wherever you need it - in the browser as an [AudioWorklet](https://developer.mozilla.org/en-US/docs/Web/API/AudioWorklet), as a standalone native backend, or embedded directly into the [BEAM](https://www.erlang.org/) via a NIF.

### Core

- **scsynth compatible** - full OSC command compatibility with SuperCollider's scsynth for synthesis, scheduling, buffers, groups and nodes. A small number of commands are unavailable or replaced — see the [command reference](docs/SCSYNTH_COMMAND_REFERENCE.md) for details.
- **Malloc-free audio path** - zero memory allocation or blocking on the audio thread. All AudioWorklet memory is pre-allocated and managed.
- **Pre-scheduler** - dynamically growing holding bay for future OSC bundles with support for cancellation. Keeps the engine's fixed-size internal scheduler from overflowing.
- **Cold-swap recovery** - multiple levels of restart from quick resume to full engine rebuild, all with automatic state restoration. Synthdefs, buffers and custom module state are captured and restored transparently.
- **Upstream compatible** - structurally independent but kept in sync with the official SuperCollider scsynth server. All upstream changes cleanly applied.
- **Tested** - 1400+ tests across web, native and NIF targets run on every release.

### Web

- **Dual transport** - SharedArrayBuffer mode for low-latency performance, postMessage mode for zero-config CDN deployment. Both are first-class and fully tested.
- **Mobile resilient** - suspend, resume and worklet death detection. Survives phone focus loss, tab backgrounding and browser worklet eviction with automatic state restoration.
- **Observable** - real-time telemetry across the full pipeline: ring buffer usage, scheduler depth, late bundles, audio health and glitch detection.
- **Multiple clients** - give any Web Worker its own OscChannel to the AudioWorklet with automatic pre-scheduler routing for far-future events. Each channel carries a source ID visible in the aggregated OSC log.
- **Zero config via CDN** - postMessage mode requires no special server headers. Works directly from CDNs such as unpkg.
- **Hosted on npm** - available as `supersonic-scsynth` with separate packages for the WASM core, synthdefs and samples.

### Native

- **Live device and driver switching** - hot-swap audio devices and drivers at runtime. Rate mismatches automatically escalate to cold swap with full state recovery.
- **Headless mode** - timer-driven audio processing without audio hardware. Platform-native high-resolution timers for CI and container deployments.
- **UDP OSC server** - drop-in scsynth replacement with device, driver, input and recording control via `/supersonic/*` commands.

### NIF

- **BEAM embedded** - scsynth as an Erlang/Elixir NIF with a clean OSC binary interface. Same protocol boundary as web and native.
- **Dirty scheduler aware** - engine initialisation runs on a dirty IO scheduler so it never blocks normal BEAM schedulers.
- **PID-based notifications** - register an Erlang process to receive OSC replies and debug output asynchronously via `enif_send()`.

## Demo

Try the live demo: [**sonic-pi.net/supersonic/demo.html**](https://sonic-pi.net/supersonic/demo.html)

## Getting Started

SuperSonic can be fetched remotely via CDN, locally via npm or self-built.

### CDN

```html
<script type="module">
  import { SuperSonic } from "https://unpkg.com/supersonic-scsynth@latest";

  const sonic = new SuperSonic({
    baseURL: "https://unpkg.com/supersonic-scsynth@latest/dist/",
    coreBaseURL: "https://unpkg.com/supersonic-scsynth-core@latest/",
    sampleBaseURL: "https://unpkg.com/supersonic-scsynth-samples@latest/samples/",
    synthdefBaseURL: "https://unpkg.com/supersonic-scsynth-synthdefs@latest/synthdefs/",
  });
  await sonic.init();
  await sonic.loadSynthDef("sonic-pi-prophet");
  sonic.send("/s_new", "sonic-pi-prophet", -1, 0, 0, "note", 60);
</script>
```

### npm / Bundler

```javascript
import { SuperSonic } from "supersonic-scsynth";

const sonic = new SuperSonic({
  baseURL: "/assets/supersonic/", // Where you serve the WASM/workers
});
await sonic.init();
await sonic.loadSynthDef("sonic-pi-prophet");
sonic.send("/s_new", "sonic-pi-prophet", -1, 0, 0, "note", 60);
```

For the full list of configuration options, see the [API Reference](docs/API.md#constructor-options). For installation options see the [Installation Guide](docs/INSTALLATION.md). Once installed, head to the [Quick Start](docs/QUICKSTART.md) to make your first sound.


## Documentation

- [Installation](docs/INSTALLATION.md) - CDN, npm, self-hosting, browser requirements
- [Quick Start](docs/QUICKSTART.md) - Boot and play your first synth
- [API Reference](docs/API.md) - Methods, callbacks, and configuration
- [Communication Modes](docs/MODES.md) - SAB vs postMessage, server configuration
- [scsynth Command Reference](docs/SCSYNTH_COMMAND_REFERENCE.md) - OSC commands for controlling scsynth
- [Workers Guide](docs/WORKERS.md) - Send OSC directly from Web Workers and AudioWorklets for the lowest latency.
- [Metrics](docs/METRICS.md) - Performance monitoring and debugging
- [Building from Source](docs/BUILDING.md) - WASM, native (JUCE), and NIF (Erlang/Elixir) builds

## Support

SuperSonic is brought to you by Sam Aaron. Please consider joining the community of supporters enabling Sam's work on creative coding projects like this, [Sonic Pi](https://sonic-pi.net) and [Tau5](https://tau5.live).

- [Patreon](https://patreon.com/samaaron)
- [GitHub Sponsors](https://github.com/sponsors/samaaron)

## License

See [LICENSE](LICENSE) for details.

SuperSonic's GPL-licensed audio engine (derived from SuperCollider's scsynth) is cleanly separated from MIT-licensed client code on every platform. All communication crosses an OSC protocol boundary — no engine types, data structures, or function calls leak into client code.

| Platform | GPL engine | MIT client | Boundary |
| -------- | ---------- | ---------- | -------- |
| **Web** | WASM + AudioWorklet (`supersonic-scsynth-core`) | JS API (`supersonic-scsynth`) | postMessage / SharedArrayBuffer ring buffers |
| **Native** | Standalone executable | N/A (UDP OSC) | Network socket |
| **NIF** | Shared library | Erlang module (`src/nif/supersonic.erl`) | NIF call passing opaque OSC binaries |

Your application code interacts only with the MIT-licensed client APIs and is not intended to be a derivative work of the GPL components.

**Native server note:** The native executable and NIF shared library contain both SuperCollider-derived code (GPL) and JUCE (GPL). The resulting binaries are GPL-3.0-or-later and any application that embeds or links them is also subject to the GPL.

**Web bundler note:** This isolation depends on the GPL code remaining a separate package loaded at runtime. If `supersonic-scsynth-core` is bundled into your application by a JavaScript bundler (webpack, Rollup, esbuild, etc.), the result is a single combined work and the GPL applies to the entire bundle.

**npm packages:**

| Package                        | License          | Contains                  |
| ------------------------------ | ---------------- | ------------------------- |
| `supersonic-scsynth`           | MIT              | JS client API + workers   |
| `supersonic-scsynth-core`      | GPL-3.0-or-later | WASM engine + AudioWorklet|
| `supersonic-scsynth-synthdefs` | MIT              | Synth definitions         |
| `supersonic-scsynth-samples`   | CC0              | Audio samples             |

## Credits

Based on [SuperCollider](https://supercollider.github.io/) by James McCartney and the SuperCollider community. This AudioWorklet port was inspired by Hanns Holger Rutz who started the first port of scsynth to WASM and Dennis Scheiba who continued this work. Thank you to everyone in the SuperCollider community!
