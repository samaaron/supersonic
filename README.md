> **Note: Still Alpha Status**: _SuperSonic is in active development._ The API may continue to evolve, but the core synthesis engine is solid and ready for experimentation. _Feedback and ideas are most welcome._

```
░█▀▀░█░█░█▀█░█▀▀░█▀▄░█▀▀░█▀█░█▀█░▀█▀░█▀▀
░▀▀█░█░█░█▀▀░█▀▀░█▀▄░▀▀█░█░█░█░█░░█░░█░░
░▀▀▀░▀▀▀░▀░░░▀▀▀░▀░▀░▀▀▀░▀▀▀░▀░▀░▀▀▀░▀▀▀
```

Back in the late 90s James McCartney designed a suite of live programming environments called [SuperCollider](https://en.wikipedia.org/wiki/SuperCollider). These were systems that had both programming languages and audio runtimes designed explicitly for live realtime modification at every level - from high abstract notions at the programming language level down to the low-level synthesis components of the audio chain.

One of the many gifts from this work is **scsynth** - the core synthesis engine James created for version 3 of SuperCollider. It was at this point when he _formally separated the language from the synth engine_.

This formal split made it possible to use **scsynth**'s powerful audio synthesis capabilities with any existing - or yet to exist - language and context.

This is SuperSonic. All the power of **scsynth** in your web browser.

# Welcome to SuperSonic

**SuperSonic** is [SuperCollider](https://supercollider.github.io/)'s powerful audio synthesis engine **scsynth** running in the browser as an [AudioWorklet](https://developer.mozilla.org/en-US/docs/Web/API/AudioWorklet).

Highlights:

- _AudioWorklet_ - runs in a dedicated high priority audio thread
- _WebAssembly_ - scsynth's original C++ code compiled for the web
- _OSC API_ - talk to the scsynth server through its native OSC API
- _Zero Config via CDN_ - no installation necessary - works directly from CDNs such as unpkg.
- _Optional SAB mode_ - can use a SharedArrayBuffer (SAB) for lower latency and reduced jitter with internal comms. Requires COOP/COEP headers to enable browsers to use the SAB


## Demo

Try the live demo: **[sonic-pi.net/supersonic/demo.html](https://sonic-pi.net/supersonic/demo.html)**


## Documentation

- [API Reference](docs/API.md) - Methods, callbacks, and configuration
- [Server Command Reference](docs/SERVER_COMMAND_REFERENCE.md) - OSC commands for controlling scsynth
- [Deployment](docs/DEPLOYMENT.md) - CDN, self-hosting, browser requirements
- [Metrics](docs/METRICS.md) - Performance monitoring and debugging
- [Building from Source](docs/BUILDING.md) - Compiling the WASM yourself


## Getting Started

In order to use SuperSonic, you need to first install it, configure it, boot it **then play**. Luckily these are all really easy. We'll go through each in turn:

1. Install
2. Configure
3. Boot & Play

### 1. Install [Easy - CDN]

Import SuperSonic directly from a CDN such as unpkg for the simplest way to get started:

```javascript
import { SuperSonic } from "https://unpkg.com/supersonic-scsynth@latest";
```

### 1. Install [Advanced - Self-Hosted]

You can also host the source yourself:

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

Then import from this directory:

```javascript
import { SuperSonic } from "./supersonic/supersonic.js";
```

### 2. Configure [Easy - Use Defaults]

If you're using SuperSonic's bundled samples and synthdefs, then no config is necessary:

```javascript
const supersonic = new SuperSonic();
```

### 2. Configure [Advanced - In Constructor]

If you want to point to your own assets, you can configure this when you create your SuperSonic instance:

A. Set a base URL (derives subdirectories automatically)
```javascript
const supersonic = new SuperSonic({
  baseURL: "/audio/supersonic/"
  // Derives: /audio/supersonic/workers/, /audio/supersonic/wasm/, etc.
});
```

B. Override individual paths
```javascript
const supersonic = new SuperSonic({
  workerBaseURL: "/my-workers/",
  wasmBaseURL: "/my-wasm/",
  synthdefBaseURL: "/my-synthdefs/",
  sampleBaseURL: "/my-samples/"
});
```

C. Enable SAB mode for lower latency (requires COOP/COEP headers)
```javascript
const supersonic = new SuperSonic({
  mode: "sab"  // default is "postMessage"
});
```

D. Enable debug logging
```javascript
const supersonic = new SuperSonic({
  debug: true  // logs scsynth output, OSC in/out to console
});
```

E. Configure scsynth server options
```javascript
const supersonic = new SuperSonic({
  scsynthOptions: {
    numBuffers: 4096,           // max audio buffers (default: 1024)
    numAudioBusChannels: 256,   // audio buses (default: 128)
    realTimeMemorySize: 16384   // RT memory in KB (default: 8192)
  }
});
```
See [API Reference](docs/API.md) for all available options.

### 3. Boot & Play

**Web browsers require you to press a button or make an explicit action before audio can start.**

The easiest way to boot SuperSonic is from a boot button handler. Consider we have the following HTML buttons:

```html
<button id="boot-btn">boot</button>
<button id="trig-btn">trigger</button>
```

We can then use the boot button for booting SuperSonic and the trigger button to trigger a synth:

```javascript
const bootBtn = document.getElementById("boot-btn");
const trigBtn = document.getElementById("trig-btn");

bootBtn.onclick = async () => {
  await supersonic.init();
  await supersonic.loadSynthDefs(["sonic-pi-prophet"]);
};

trigBtn.onclick = async () => {
  supersonic.send("/s_new", "sonic-pi-prophet", -1, 0, 0, "note", 28, "release", 8, "cutoff", 70);
};
```

See `example/simple.html` for a minimal working example.


## Support

SuperSonic is brought to you by Sam Aaron. Please consider joining the community of supporters enabling Sam's work on creative coding projects like this, [Sonic Pi](https://sonic-pi.net) and [Tau5](https://tau5.live).

- [Patreon](https://patreon.com/samaaron)
- [GitHub Sponsors](https://github.com/sponsors/samaaron)


## License

GPL v3 - This is a derivative work of SuperCollider


## Credits

Based on [SuperCollider](https://supercollider.github.io/) by James McCartney and the SuperCollider community. This AudioWorklet port was inspired by Hanns Holger Rutz who started the first port of scsynth to WASM and Dennis Scheiba who continued this work. Thank you to everyone in the SuperCollider community!
