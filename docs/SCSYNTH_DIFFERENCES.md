# SuperSonic vs scsynth Differences

This document describes the key differences between SuperSonic and the original SuperCollider scsynth server.

## Overview

SuperSonic is a port of scsynth designed to run inside a browser's AudioWorklet. While it maintains compatibility with most of the scsynth API, certain features are unfortunately unavailable due to the constraints of the AudioWorklet environment.

## AudioWorklet Constraints

The AudioWorklet environment imposes strict limitations:

| Constraint | Impact |
|------------|--------|
| **No thread spawning** | Single-threaded execution only |
| **No malloc in audio thread** | All memory must be pre-allocated |
| **No filesystem access** | No disk I/O operations |
| **No network sockets** | No UDP/TCP communication |
| **No DOM access** | No mouse/keyboard state queries |
| **No main() entry point** | SuperSonic's scsynth doesn't "run" it gets called.  |

These constraints mean that certain scsynth features simply cannot work in the browser environment and this document covers the differences.

---

## Unsupported UGens

A UGen is the main building block for synth definitions. A synth is effectively a tree of connected UGens - which runs in the scsynth node tree. If a UGen isn't available - then any synth definition which uses that UGen can't be used.

### What happens when loading a synthdef with an unsupported UGen?

When you attempt to load a synthdef that references an unsupported UGen (via `/d_recv` or `loadSynthDef()`):

1. The load **fails** - the synthdef is not added to the server
2. A `/fail` message is sent: `/fail /d_recv "UGen 'MouseX' not installed."`
3. No `/done` message is sent
4. The error is also logged to debug output

This allows you to programmatically detect when a synthdef can't be loaded and handle it appropriately.

**Note:** This is slightly different beheviour to the original scsynth, which silently sends `/done` even when synthdef loading fails due to a missing UGen.

### Unsupported UGen List

The following UGens from standard SuperCollider are not available in SuperSonic:

### User Interface UGens

These require DOM/window access which is unavailable in AudioWorklet:

| UGen | Description |
|------|-------------|
| `MouseX` | Horizontal mouse position |
| `MouseY` | Vertical mouse position |
| `MouseButton` | Mouse button state |
| `KeyState` | Keyboard key state |

**Workaround:** Use control buses updated from JavaScript based on mouse/keyboard events:
```javascript
document.addEventListener('mousemove', (e) => {
  const x = e.clientX / window.innerWidth;
  supersonic.send("/c_set", 0, x); // Update control bus 0
});
```

### Disk I/O UGens

These require filesystem access:

| UGen | Description |
|------|-------------|
| `DiskIn` | Stream audio from disk |
| `DiskOut` | Record audio to disk |
| `VDiskIn` | Variable-rate disk streaming |

**Workaround:** Pre-load samples into buffers using `loadSample()` or `/b_allocFile`.

### Network/Link UGens

These require network socket access:

| UGen | Description |
|------|-------------|
| `LinkTempo` | Ableton Link tempo sync |
| `LinkPhase` | Ableton Link phase |
| `LinkJump` | Ableton Link position jump |

**Note:** These were added to upstream SuperCollider in [PR #6947](https://github.com/supercollider/supercollider/pull/6947) and are intentionally excluded from SuperSonic.

### Bela Hardware UGens

These are specific to Bela embedded hardware:

| UGen | Description |
|------|-------------|
| `AnalogIn` | Bela analog input |
| `AnalogOut` | Bela analog output |
| `DigitalIn` | Bela digital input |
| `DigitalOut` | Bela digital output |
| `DigitalIO` | Bela digital I/O |
| `MultiplexAnalogIn` | Bela multiplexed analog input |
| `BelaScopeOut` | Bela oscilloscope output |

### Machine Learning / Analysis UGens

These UGens are not currently compiled into SuperSonic:

| UGen | Description |
|------|-------------|
| `BeatTrack` | Beat tracking |
| `BeatTrack2` | Improved beat tracking |
| `KeyTrack` | Musical key detection |
| `Loudness` | Perceptual loudness |
| `MFCC` | Mel-frequency cepstral coefficients |
| `Onsets` | Onset detection |
| `SpecFlatness` | Spectral flatness measure |
| `SpecPcile` | Spectral percentile |
| `SpecCentroid` | Spectral centroid |

**Note:** These could potentially be added in the future as they don't have fundamental AudioWorklet incompatibilities. If you need these, please [open an issue](https://github.com/samaaron/supersonic/issues).

---

## Unsupported OSC Commands

### Filesystem Commands

No filesystem in browser, so file-based commands aren't available:

| Command | Alternative |
|---------|-------------|
| `/d_load` | `loadSynthDef()` or `/d_recv` with bytes |
| `/d_loadDir` | `loadSynthDefs()` |
| `/b_read` | `loadSample()` |
| `/b_readChannel` | `loadSample()` |
| `/b_allocRead` | `loadSample()` or `/b_allocFile` |
| `/b_allocReadChannel` | `loadSample()` (channel selection not supported) |
| `/b_write` | Not available |
| `/b_close` | Not available |

### Scheduling and Control Commands

| Command | Reason / Alternative |
|---------|---------------------|
| `/clearSched` | Use `cancelAllScheduled()` or fine-grained `cancelTag()`, `cancelSession()`, `cancelSessionTag()` |
| `/error` | SuperSonic always enables error notifications |
| `/quit` | Use `destroy()` to shut down SuperSonic |

### Plugin Commands

| Command | Status |
|---------|--------|
| `/cmd` | No commands currently registered |
| `/u_cmd` | No UGens currently define commands |

### Buffer Commands

| Command | Reason |
|---------|--------|
| `/b_setSampleRate` | WebAudio automatically resamples buffers to context sample rate |

For full details, see [SCSYNTH_COMMAND_REFERENCE.md](SCSYNTH_COMMAND_REFERENCE.md#unsupported-commands).

---

## SuperSonic Extensions

SuperSonic adds functionality not present in standard scsynth:

### `/b_allocFile` - Inline Audio Loading

Load audio from inline file data without needing a URL:

```javascript
const response = await fetch("sample.flac");
const fileBytes = new Uint8Array(await response.arrayBuffer());
supersonic.send("/b_allocFile", 0, fileBytes);
```

Supports FLAC, WAV, OGG, MP3, and any format the browser's `decodeAudioData()` handles.

### JavaScript API

SuperSonic provides a high-level JavaScript API that wraps the OSC protocol:

| Method | Description |
|--------|-------------|
| `loadSynthDef(name)` | Fetch and load a synthdef by name |
| `loadSynthDefs(names)` | Load multiple synthdefs |
| `loadSample(bufnum, url)` | Fetch and load audio into a buffer |
| `cancelAllScheduled()` | Cancel all scheduled OSC bundles |
| `cancelTag(tag)` | Cancel bundles with specific tag |
| `cancelSession(session)` | Cancel bundles from specific session |
| `destroy()` | Clean shutdown |

### Dual Communication Modes

SuperSonic supports two communication modes between JavaScript and the AudioWorklet:

1. **SharedArrayBuffer (SAB)** - Lower latency, requires specific server headers
2. **postMessage** - Works everywhere, slightly higher latency

Both modes are fully supported and tested.

---

## Architectural Differences

### Threading Model

| scsynth | SuperSonic |
|---------|------------|
| Multi-threaded (audio thread + NRT thread + network thread) | Single-threaded (AudioWorklet) |
| Thread-safe queues for OSC | SharedArrayBuffer ring buffers |
| Async command processing on NRT thread | All commands processed synchronously |

### Memory Model

| scsynth | SuperSonic |
|---------|------------|
| Dynamic allocation via malloc | Pre-allocated memory pool |
| Grows as needed | Fixed size at boot |
| OS-managed | WASM linear memory |

### OSC Transport

| scsynth | SuperSonic |
|---------|------------|
| UDP/TCP network sockets | SharedArrayBuffer or postMessage |
| Multiple network clients | Single JavaScript client |
| External OSC sources | Only local JavaScript |

---

## Summary: What Works

The vast majority of scsynth functionality works in SuperSonic:

- All standard oscillators (SinOsc, Saw, Pulse, etc.)
- All filters (LPF, HPF, BPF, RLPF, etc.)
- All envelopes (EnvGen, Linen, etc.)
- All noise generators (WhiteNoise, PinkNoise, etc.)
- All delays (DelayN, DelayL, CombN, AllpassN, etc.)
- All FFT/spectral UGens (FFT, IFFT, PV_*)
- All triggers (Impulse, Dust, Trig, etc.)
- All math/utility UGens (Mix, Pan2, etc.)
- Buffers and buffer playback (PlayBuf, BufRd, etc.)
- Control and audio buses
- Groups and node ordering
- All standard OSC commands for synth control

If a UGen or command isn't listed in the unsupported sections above, it should work as expected.
