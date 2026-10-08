# SuperSonic vs scsynth Differences

This document describes the key differences between SuperSonic and the original SuperCollider scsynth server.

## Overview

SuperSonic is a port of scsynth that runs on three hosts: inside a browser's AudioWorklet (the web), as a standalone native server on macOS, Windows and Linux, and as a BEAM NIF. It keeps most of the scsynth API. Some features are unavailable, some because of the AudioWorklet and some because the engine has no files on any host. Each difference below says which hosts it applies to; one that names none applies to all three.

## Constraints

| Constraint | Hosts | Impact |
|------------|-------|--------|
| **No threads of its own** | All | scsynth runs on the audio thread, and its asynchronous commands run inline there |
| **No malloc in audio thread** | All | Memory comes from pools set up before the engine runs |
| **No filesystem access** | All | The engine opens no files: the web client, and `supersonic::Commands` on the native server and the NIF, read and decode them for it (see [Filesystem Commands](#filesystem-commands)) |
| **No network sockets** | Web | OSC comes from JavaScript only. The native server takes UDP, TCP and more |
| **No DOM access** | Web | No mouse/keyboard state queries |
| **No main() entry point** | All | SuperSonic's scsynth doesn't "run": it gets called |

These constraints mean that certain scsynth features cannot work, and this document covers the differences.

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

**Note:** This is slightly different behaviour to the original scsynth, which silently sends `/done` even when synthdef loading fails due to a missing UGen.

### Unsupported UGen List

The following UGens from standard SuperCollider are not available in SuperSonic:

### User Interface UGens

These read the mouse and keyboard from the window system. They are not built on any host, and an AudioWorklet has no window to read in any case:

| UGen | Description |
|------|-------------|
| `MouseX` | Horizontal mouse position |
| `MouseY` | Vertical mouse position |
| `MouseButton` | Mouse button state |
| `KeyState` | Keyboard key state |

**Workaround:** Use control buses, set by the client from mouse/keyboard events with `/c_set`. On the web:
```javascript
document.addEventListener('mousemove', (e) => {
  const x = e.clientX / window.innerWidth;
  supersonic.send("/c_set", 0, x); // Update control bus 0
});
```

### Disk I/O UGens

These stream from and to files, and the engine has no files on any host:

| UGen | Description |
|------|-------------|
| `DiskIn` | Stream audio from disk |
| `DiskOut` | Record audio to disk |
| `VDiskIn` | Variable-rate disk streaming |

**Workaround:** Pre-load samples into buffers. On the web use `loadSample()` or `/b_allocFile`; on the native server, `/b_allocRead` (see [Filesystem Commands](#filesystem-commands)).

### Link UGens

| UGen | Status |
|------|--------|
| `LinkTempo` | Built, read-only: the session tempo in cycles per second |
| `LinkPhase` | Built, read-only: the phase within the quantum given as its input |
| `LinkJump` | Not available |

These were added to upstream SuperCollider in [PR #6947](https://github.com/supercollider/supercollider/pull/6947). In SuperSonic `LinkTempo` and `LinkPhase` read clockwork's session clock rather than Ableton Link itself, so they work on every host. Where Link is running (natively), the session clock is Link's; elsewhere, including the web, it is clockwork's own. Upstream's write side is gone: `LinkTempo` cannot set the tempo and `LinkJump` is not built. Change the session's tempo with clockwork's `/clockwork/clock/` verbs instead.

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

**Note:** These could potentially be added in the future as they don't have fundamental incompatibilities with any host. If you need these, please [open an issue](https://github.com/samaaron/supersonic/issues).

---

## OSC Commands Some Hosts Cannot Serve

On the web, nothing is refused by the client: every verb goes to the engine, and the
engine's own `/fail` is the answer for the ones it cannot serve. (Until
2026-09-13 the client refused these itself with a friendlier message; the
engine's answer is the honest one, and `/error -1`/`-2` — quieting one
bundle's failures — is a standard scsynth idiom the refusal forbade.) The
exceptions are `/b_alloc`, `/b_allocRead`, `/b_allocReadChannel` and
`/b_allocFile`, which the client rewrites to `/b_allocPtr` before they are
sent.

### Filesystem Commands

The engine has no files on any host. Each host deals with the file verbs differently:

| Command | Web | Native server and NIF |
|---------|-----|-----------------------|
| `/d_load` | Refused with `/fail`: use `loadSynthDef()`, or `/d_recv` with the bytes | Reads the file and sends `/d_recv` |
| `/d_loadDir` | Refused with `/fail`: use `loadSynthDefs()` | Reads each file and sends `/d_recv` |
| `/b_read` | Fails: use `loadSample()` | Decodes the file |
| `/b_readChannel` | Fails: use `loadSample()` | Decodes the file |
| `/b_allocRead` | Rewritten to `/b_allocPtr`: the client fetches and decodes the file | Decodes the file |
| `/b_allocReadChannel` | Rewritten to `/b_allocPtr`, with channel selection | Decodes the file |
| `/b_write` | Fails | Encodes the file (`leaveOpen` is not available) |
| `/b_close` | Fails | Fails |

The native server and the NIF send every command through `supersonic::Commands` (`front/supersonic_commands.h`), which sits between their clients and the engine. It does the file work on a thread of its own and answers with scsynth's replies, such as `/done /b_allocRead bufnum`. On the web, the path given to `/b_allocRead` is fetched over HTTP, through `sampleBaseURL` when it is a bare file name.

A file command that reaches the engine itself — on the web, or over the native server's `--shm-commands` plane — is refused with `/fail`, naming what to send instead.

### Scheduling and Control Commands

| Command | Note |
|---------|------|
| `/clearSched` | Drops the bundles clockwork holds for scsynth (up to v0.89.0 it left them to fire). On the web, `purge()` also drops what is still in the IN ring and everything else the scheduler holds |
| `/error` | Works as in scsynth: `/error 0` silences `/fail`; on the web, the client's own waits then time out |
| `/quit` | Native server: answers `/done /quit` and shuts down, as scsynth does. NIF: fails, pointing at `clockwork:stop/0`. Web: fails with "not supported in SuperSonic - use destroy() instead"; `destroy()` shuts the engine down |

### Plugin Commands

| Command | Status |
|---------|--------|
| `/cmd` | Only upstream's demo command, `pluginCmdDemo`, is registered |
| `/u_cmd` | Only upstream's demo UGen, `UnitCmdDemo`, defines commands (`setValue`, `testCommand`) |

### Buffer Commands

| Command | Reason |
|---------|--------|
| `/b_setSampleRate` | Not a command of the engine on any host. A buffer's sample rate is set when it is allocated |

For full details, see [SCSYNTH_COMMAND_REFERENCE.md](SCSYNTH_COMMAND_REFERENCE.md#unsupported-commands).

---

## OSC Command Behaviour Differences

Commands that exist in scsynth but behave differently in SuperSonic.

### `/notify` is idempotent

In upstream scsynth, sending `/notify 1` from a client that is **already**
registered replies with `/fail "notify: already registered"`. In SuperSonic it
instead replies `/done /notify <clientID> <maxClients>` — the same success
reply as a first-time registration, returning the existing client ID.

Why: SuperSonic **preserves notify registrations across an audio-device rebuild**
(a sample-rate or driver change destroys and rebuilds the scsynth World, which
would otherwise empty the registered-client list and silently stop `/n_go` /
`/n_end` delivery). The registrations are captured before the rebuild and
restored after it. A host that *also* re-sends `/notify` after such a rebuild
(harmlessly, not knowing the engine already restored it) must therefore get a
success, not an error — so re-registration is idempotent. This matches the
original code's own intent (its comment already read "already in table — don't
fail though").

SuperSonic adds functionality not present in standard scsynth:

### Zombie Synth Prevention

When RT memory is exhausted during UGen construction, upstream scsynth leaves dead synth nodes that never free themselves — no `DoneAction` fires because all units are marked as done at construction time. These "zombie" nodes consume RT memory indefinitely and can prevent all future synth creation.

SuperSonic detects when all units in a synth are dead at construction time and schedules the node for cleanup automatically, using the same mechanism as `DoneAction=2` (`freeSelf`). This is a no-op when any unit survived construction, preserving upstream behaviour exactly.

### `/b_allocFile` - Inline Audio Loading (web)

On the web, load audio from inline file data without needing a URL:

```javascript
const response = await fetch("sample.flac");
const fileBytes = new Uint8Array(await response.arrayBuffer());
supersonic.send("/b_allocFile", 0, fileBytes);
```

Supports FLAC, WAV, OGG, MP3, and any format the browser's `decodeAudioData()` handles. The client decodes the file and sends `/b_allocPtr` in its place. The native server and the NIF have no `/b_allocFile`.

### JavaScript API

On the web, SuperSonic provides a high-level JavaScript API that wraps the OSC protocol:

| Method | Description |
|--------|-------------|
| `loadSynthDef(name)` | Fetch and load a synthdef by name |
| `loadSynthDefs(names)` | Load multiple synthdefs |
| `loadSample(bufnum, url)` | Fetch and load audio into a buffer |
| `purge()` | Drop every scheduled bundle and everything still waiting in the IN ring |
| `destroy()` | Clean shutdown |

### Dual Communication Modes

On the web, SuperSonic supports two communication modes: **postMessage** (works everywhere) and **SAB** (lower latency, requires server headers). The default is SAB when the page is cross-origin isolated (`crossOriginIsolated`) and postMessage otherwise. Both are fully supported and tested. See [Communication Modes](MODES.md) for details.

---

## UGen Behaviour Differences

### Signed squared/cubed envelope warps

Upstream scsynth computes the `\sqr` (shape 6) and `\cub` (shape 7) envelope
warps in root space on the raw levels (`sqrt(level)` / `pow(level, 1/3)`), so
any segment touching a negative level produces NaN: the envelope sticks, clamps
to zero, or emits NaN samples. A `pan_slide` from `-1` with
`pan_slide_shape: 6` never moves (sonic-pi#169).

SuperSonic uses the odd (signed) extensions instead: `copysign(sqrt(|x|), x)`
with `y * |y|` reconstruction for squared, and `cbrt` for cubed. Non-negative
envelopes are bit-identical to upstream; negative ranges ramp smoothly with the
same eased feel mirrored below zero.

Guarded by `#ifdef CLOCKWORK_GUEST` in `LFUGens.cpp` (EnvGen, `GET_ENV_VAL`) and
`DemandUGens.cpp` (demand-rate envelopes), with upstream code preserved in the
`#else` branches. Regression spec: `test/envgen_signed_shapes.spec.mjs`.

### Exponential envelope warp with zero endpoints

Upstream's exponential warp (shape 2, `\exp`) computes
`grow = pow(end/start, 1/n)` then `level *= grow`, so a segment anchored at
zero sticks at silence for its whole duration and jumps at the boundary — an
audible click for any amp envelope, since those start and end at 0
(sonic-pi#881).

SuperSonic substitutes ±1e-4 (-80dB, inaudible) for zero endpoints, borrowing
the other endpoint's sign, so zero-anchored exponential segments ramp
smoothly. Envelopes with non-zero endpoints are bit-identical to upstream.
Sign-crossing exponentials remain undefined, as upstream.

Same guard convention as above. Regression spec: `test/envgen_exp_zero.spec.mjs`.

---

## Architectural Differences

### Threading Model

| scsynth | SuperSonic |
|---------|------------|
| Multi-threaded (audio thread + NRT thread + network thread) | scsynth is single-threaded on every host: it runs on the audio thread (the AudioWorklet on the web; natively the device callback, or clockwork's headless thread when there is no device). Natively, clockwork's control and network threads sit around it |
| Thread-safe queues for OSC | Shared-memory ring buffers (on the web, in postMessage mode, the worklet writes posted messages onto the same ring) |
| Async command processing on NRT thread | All commands processed synchronously, on the audio thread |

### Memory Model

| scsynth | SuperSonic |
|---------|------------|
| Dynamic allocation via malloc | Pre-allocated memory pools, on every host |
| Grows as needed | The real-time pool is fixed at boot. On the web, the sample-buffer pool grows as samples load, from 4 MB up to 768 MB by default |
| OS-managed | Web: WASM linear memory. Native: memory clockwork claims when the engine starts |

### OSC Transport

| scsynth | SuperSonic |
|---------|------------|
| UDP/TCP network sockets | Web: SharedArrayBuffer or postMessage. Native server: UDP by default, or TCP, a Unix socket, a named pipe or shared memory. NIF: `send_osc/1` |
| Multiple network clients | Web: a single JavaScript client. Native server: many clients. NIF: every registered process hears every reply |
| External OSC sources | Web: only local JavaScript. Native server: any OSC client that can reach its port (it listens on 127.0.0.1 unless `-B` names another address) |

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
