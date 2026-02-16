# Quick Start

You've installed SuperSonic - now let's make some sound.

We'll create a simple page with two buttons: one to boot the audio engine, and one to trigger a synth.

```html
<button id="boot-btn">boot</button>
<button id="trig-btn">trigger</button>
```

```javascript
import { SuperSonic } from "https://unpkg.com/supersonic-scsynth@latest";

const supersonic = new SuperSonic({
  baseURL: "https://unpkg.com/supersonic-scsynth@latest/dist/",
  synthdefBaseURL: "https://unpkg.com/supersonic-scsynth-synthdefs@latest/synthdefs/",
});

const bootBtn = document.getElementById("boot-btn");
const trigBtn = document.getElementById("trig-btn");

bootBtn.onclick = async () => {
  await supersonic.init();
  await supersonic.loadSynthDef("sonic-pi-prophet");
};

trigBtn.onclick = () => {
  supersonic.send("/s_new", "sonic-pi-prophet", -1, 0, 0, "note", 28, "release", 8, "cutoff", 70);
};
```

Let's break down what's happening here.


## User Interaction

Web browsers have an autoplay policy that prevents websites from making sound without user consent. Audio can only start after a user interaction like a click, tap, or keypress.

This is why we use a boot button - calling `init()` from a button handler satisfies the browser's autoplay policy and allows audio to begin.


## Creating a SuperSonic Instance

```javascript
const supersonic = new SuperSonic({
  baseURL: "https://unpkg.com/supersonic-scsynth@latest/dist/",
  synthdefBaseURL: "https://unpkg.com/supersonic-scsynth-synthdefs@latest/synthdefs/",
});
```

This creates a new SuperSonic instance configured to load assets from CDN. The `baseURL` tells SuperSonic where to find the WASM engine and workers. The `synthdefBaseURL` tells it where to find synthdef files when you call `loadSynthDef()`. The instance doesn't start the audio engine yet - it just sets up the configuration. You can pass additional options to configure transport mode, debug output, and scsynth engine settings (see [API Reference](API.md)).


## Booting the Engine

```javascript
await supersonic.init();
```

Calling `init()` boots the scsynth audio engine. Behind the scenes this:

1. Creates an AudioContext and AudioWorklet
2. Loads the WebAssembly module containing scsynth
3. Starts scsynth running in a dedicated high-priority audio thread

This is an async operation - the `await` ensures we don't proceed until the engine is ready.


## Loading a Synth Definition

```javascript
await supersonic.loadSynthDef("sonic-pi-prophet");
```

Before you can play a synth, you need to send its design to scsynth. This design is called a **synth definition** (or "synthdef") and is a recipe that describes a synth's audio graph - what oscillators, filters, and effects it uses and how they're connected.

SuperSonic comes with 127 ready-to-use synthdefs from [Sonic Pi](https://sonic-pi.net). Here we're loading `sonic-pi-prophet`, a warm polyphonic synth inspired by the Prophet-5.

__Note: you can also use SuperCollider's Desktop app to design your own synthdefs and directly import them live at runtime into your SuperSonic session.__


## Triggering a Synth

```javascript
supersonic.send("/s_new", "sonic-pi-prophet", -1, 0, 0, "note", 28, "release", 8, "cutoff", 70);
```

Now for the fun part - making sound! The `send()` method sends OSC (Open Sound Control) messages to scsynth. The `/s_new` command creates a new synth instance.

The arguments are:

|               |                      |                                        |
|---------------|----------------------|----------------------------------------|
| synthdef name | `"sonic-pi-prophet"` | Which synth to create                  |
| node ID       | `-1`                 | Let scsynth assign an ID automatically |
| add action    | `0`                  | Add to the head of the target group    |
| target        | `0`                  | The root group                         |
| params...     | `"note", 28, ...`    | Name/value pairs for synth parameters  |

The synth parameters control the sound. Here we set `note` to 28 (a low E), `release` to 8 seconds, and `cutoff` to 70 (filter brightness). Each synthdef has its own parameters - see the synthdef documentation for available options.


## Working Example

See `example/simple.html` for a complete working example you can run locally, or `example/simple_metrics.html` for the same example with a live metrics dashboard.


## Adding a Metrics Dashboard

SuperSonic includes a web component that renders a full metrics dashboard from the schema:

```html
<link rel="stylesheet" href="https://unpkg.com/supersonic-scsynth@latest/dist/metrics-dark.css" />
<script type="module" src="https://unpkg.com/supersonic-scsynth@latest/dist/metrics_component.js"></script>

<supersonic-metrics id="metrics"></supersonic-metrics>
```

Connect it after boot to start live updates:

```javascript
bootBtn.onclick = async () => {
  await supersonic.init();
  await supersonic.loadSynthDef("sonic-pi-prophet");
  document.getElementById("metrics").connect(supersonic, { refreshRate: 10 });
};
```

See [Metrics Component](METRICS_COMPONENT.md) for theming, layout control, and customisation.

## Next Steps

- [API Reference](API.md) - All methods and configuration options
- [Metrics Component](METRICS_COMPONENT.md) - Real-time metrics dashboard
- [scsynth Command Reference](SCSYNTH_COMMAND_REFERENCE.md) - OSC commands for controlling synthesis
