# Sending OSC from Workers

## Avoiding the Main Thread

When you send OSC from the main thread, it has to compete with everything else happening there - DOM updates, event handlers, animations, your application logic. For simple cases this is fine, but if you're running a sequencer, processing MIDI input, or doing other timing-sensitive work, you can end up with jitter or UI stuttering.

Web Workers let you move work off the main thread. The challenge is getting OSC from a worker to the AudioWorklet efficiently. You could send messages back to the main thread and have it forward them - but that adds latency and defeats the purpose of using a worker in the first place.

SuperSonic solves this with `OscChannel` - a transferable object that gives workers a direct line to the AudioWorklet.

## OscChannel

Normally you'd just call `supersonic.send()` - but workers are separate threads with no access to your main thread's objects. The `supersonic` instance doesn't exist in the worker's world.

You could have the worker send messages back to the main thread via `postMessage`, and have the main thread forward them to scsynth. But that puts the main thread back in the middle of every message - defeating the purpose of using a worker.

The solution is to create a direct channel from the worker to scsynth running in the AudioWorklet. SuperSonic gives you an `OscChannel` for exactly this.

You create an `OscChannel` with `createOscChannel` and then you need to **transfer** it to the worker.

Note: Transferring is critical because the core internal comms mechanisms cannot be copied to the worker with a standard `postMessage`, they must be explicitly transferred. This enables the worker to have full and unique ownership of the newly created `OscChannel`.

Here's how you create an `OscChannel` and transfer it to a worker:

```javascript
// Main thread - create and transfer
const channel = supersonic.createOscChannel();
worker.postMessage(
  { type: "init", channel: channel.transferable },
  channel.transferList
);
```

The second argument to `postMessage` is optional - when provided, it lists which objects to transfer rather than copy.

In your worker, handle this as an init message and reconstruct the channel:

```javascript
// Worker thread
import { OscChannel } from "supersonic-scsynth";

let channel = null;

self.onmessage = (event) => {
  if (event.data.type === "init") {
    channel = OscChannel.fromTransferable(event.data.channel);
  }
};

// Now you can send OSC directly to the AudioWorklet
channel.send(oscBytes);
```

Using a `type` field lets you handle different message types cleanly - for example `init`, `start`, `stop` (see the [sequencer example](#example-sequencer-worker) below).


## Multiple Workers

Each call to `createOscChannel()` returns a new channel with a unique source ID. This means you can have multiple workers all sending OSC independently - they don't need to coordinate with each other, and their messages can be traced back to their source in metrics and logs.

```javascript
const sequencerChannel = supersonic.createOscChannel(); // sourceId: 1
const lfoChannel = supersonic.createOscChannel(); // sourceId: 2
const midiChannel = supersonic.createOscChannel(); // sourceId: 3

sequencerWorker.postMessage({ channel: sequencerChannel.transferable }, sequencerChannel.transferList);
lfoWorker.postMessage({ channel: lfoChannel.transferable }, lfoChannel.transferList);
midiWorker.postMessage({ channel: midiChannel.transferable }, midiChannel.transferList);
```

## SAB vs PM Mode

TLDR; `OscChannel` works identically in both SAB and PM modes.

SuperSonic can run in one of two modes which affects how inter-worker communication happens. SAB mode uses a SharedArrayBuffer that all workers can access, and PM mode uses explicit message sending with `postMessage`.

The `OscChannel` is aware of its mode and uses the appropriate method for direct communication to the scsynth engine running in WASM. This is completely handled for you so the behaviour is identical regardless of the mode.

## Message Routing

`OscChannel.send()` automatically classifies messages and routes them appropriately:

| Message Type                       | Where It Goes          | Why                                 |
| ---------------------------------- | ---------------------- | ----------------------------------- |
| Regular messages (not bundles)     | Direct to AudioWorklet | No timing requirements              |
| Immediate bundles (timetag 0 or 1) | Direct to AudioWorklet | Execute now                         |
| Near-future bundles (within 500ms) | Direct to AudioWorklet | Close enough to buffer              |
| Late bundles (in the past)         | Direct to AudioWorklet | Execute immediately                 |
| Far-future bundles (>500ms ahead)  | Prescheduler           | Hold until closer to execution time |

The 500ms threshold is configurable via `bypassLookaheadMs` in the SuperSonic constructor.

## OscChannel API

### Methods

| Method                 | Description                                |
| ---------------------- | ------------------------------------------ |
| `send(oscBytes)`       | Send with automatic routing                |
| `sendDirect(oscBytes)` | Force direct send (bypass prescheduler)    |
| `classify(oscBytes)`   | Get routing classification without sending |
| `close()`              | Release resources                          |

### Properties

| Property       | Description                     |
| -------------- | ------------------------------- |
| `mode`         | `'sab'` or `'postMessage'`      |
| `transferable` | Data for `postMessage` transfer |
| `transferList` | Transferable objects array      |

### Static Methods

| Method                              | Description                   |
| ----------------------------------- | ----------------------------- |
| `OscChannel.fromTransferable(data)` | Reconstruct channel in worker |

## Example: Sequencer Worker

Here's a worker that runs a simple step sequencer:

```javascript
// sequencer-worker.js
import { OscChannel, osc } from "supersonic-scsynth";

let channel = null;
let running = false;
let step = 0;
let bpm = 120;

const pattern = [60, 62, 64, 65, 67, 65, 64, 62]; // Notes to play

self.onmessage = (event) => {
  const { type, data } = event.data;

  if (type === "init") {
    channel = OscChannel.fromTransferable(data.channel);
  } else if (type === "start") {
    running = true;
    step = 0;
    tick();
  } else if (type === "stop") {
    running = false;
  } else if (type === "bpm") {
    bpm = data.bpm;
  }
};

function tick() {
  if (!running || !channel) return;

  const note = pattern[step % pattern.length];
  const msg = osc.encodeMessage("/s_new", [
    "sonic-pi-beep", -1, 0, 0,
    "note", note,
    "amp", 0.5
  ]);
  channel.send(msg);

  step++;
  const msPerBeat = 60000 / bpm;
  setTimeout(tick, msPerBeat / 4); // 16th notes
}
```

## Cleanup

When you're done with a worker, you may close its OSC channel:

```javascript
// In the worker
channel.close();
```

