# SuperSonic Architecture

> **Since 0.80, SuperSonic runs on [clockwork](https://github.com/samaaron/clockwork)**
> (the git submodule at `clockwork/`). The substrate described below — the
> AudioWorklet, the shared-memory rings, the workers, the transports, the
> clock, metrics and the native device layer — is clockwork's, and
> clockwork's own docs (`clockwork/docs/`) are the reference for it. What is
> SuperSonic's is the scsynth guest (`dsp/scsynth`), the JavaScript on top of
> clockwork's client (`js/supersonic.js` extends `Clockwork`), the native
> process (`host/`, `front/`) and the packages. Files below are named by
> their path from the repository root.

SuperSonic ports SuperCollider's scsynth audio engine to three hosts: the web, where it runs in a WebAssembly AudioWorklet; a standalone native server on macOS, Windows and Linux; and a BEAM NIF. This document covers the web host and the design decisions behind it. The native server is described in [NATIVE.md](NATIVE.md) and the NIF in [NIF.md](NIF.md). Where a section also holds for those hosts, it says so.

## Core Challenges

### 1. AudioWorklet Constraints
On the web, the WASM scsynth runs inside an AudioWorklet with strict requirements:
- No thread spawning
- No malloc (memory must be pre-allocated)
- No I/O
- No main() entry point
- No automatic C++ initializer calls

The first three are the rule for the audio thread on every host: natively the device callback (or, with no device, clockwork's headless thread) drives the engine, and the guest may not allocate, lock or do I/O there either (`clockwork/src/dsp_api.h`).

The original scsynth was multi-threaded with separate threads for I/O and audio graph calculations. On every host SuperSonic's scsynth is single-threaded instead: it runs on the audio thread, and its asynchronous commands run their stages inline there (`mRealTime` is false). On the web, OSC travels between JavaScript and the worklet through SharedArrayBuffer memory, or by postMessage where there is none.

### 2. Memory Management Without SAB
SharedArrayBuffer requires COOP/COEP headers, preventing CDN deployment. We created **postMessage mode** as an alternative that works anywhere but with slightly higher latency.

## Two Communication Modes

| Mode | Mechanism | Deployment | Latency |
|------|-----------|------------|---------|
| **SAB** | SharedArrayBuffer + ring buffers | Self-hosted (COOP/COEP headers) | Lower |
| **PM** | postMessage | CDN-compatible | Higher |

Both modes are first-class citizens. All tests must pass in both modes. See [Communication Modes](MODES.md) for configuration details and server setup.

## NTP Time and Clock Synchronization

### The Problem

OSC bundles carry NTP timestamps indicating when they should execute. All timestamps throughout SuperSonic are NTP-based (seconds since 1900-01-01), on every host. On the web, however, the AudioWorklet has no access to `performance.now()` or the system clock - it only receives the audio clock timestamp (`currentTime`) passed into `process()`.

The AudioWorklet must translate between audio clock and NTP to know when to dispatch scheduled bundles to scsynth. The rest of this section is about that translation, so it is web-only: natively there is no AudioContext, and each audio callback takes the block's NTP time from the system clock, smoothed from one callback to the next.

### OSC Bundle Timestamps

- **Timetag 0 or 1**: Execute immediately
- **Any other value**: NTP timestamp (seconds since 1900-01-01)

NTP time is calculated from the system clock:
```
ntpTime = (performance.timeOrigin + performance.now()) / 1000 + NTP_EPOCH_OFFSET
```

where `NTP_EPOCH_OFFSET = 2208988800` (seconds between 1900 and 1970).

### Clock Translation

At AudioContext boot, we record the **NTP start time** - the NTP timestamp when `audioContext.currentTime` was zero:

```
ntpStartTime = currentNTP - audioContext.currentTime
```

The AudioWorklet can then convert audio time to NTP:

```
currentNTP = audioContextTime + ntpStartTime + driftOffset + clockOffset
```

`clockOffset` is zero unless set with `setClockOffset()`, for syncing with another system.

### Drift Management

The audio clock and system clock drift relative to each other (hardware crystals aren't perfect). At typical 100ppm drift, clocks can diverge ~0.1ms per second.

We measure and correct for drift:

1. **Main thread** periodically (every 1000ms) compares expected vs actual `contextTime`:
   ```
   expectedContextTime = currentNTP - ntpStartTime
   driftUs = (expectedContextTime - actualContextTime) * 1000000
   ```

2. **Drift offset** is written to shared memory (SAB mode) or sent via postMessage (PM mode), in microseconds

3. **AudioWorklet** applies drift correction when converting timestamps

### Time Data Flow

```
Main Thread                          AudioWorklet
    │                                     │
    │  ntpStartTime (at boot)             │
    ├────────────────────────────────────▶│
    │                                     │
    │  driftOffset (every 1s)             │
    ├────────────────────────────────────▶│
    │                                     │
    │                     currentTime ────┤
    │                           +         │
    │                   ntpStartTime      │
    │                           +         │
    │                    driftOffset      │
    │                           =         │
    │                     currentNTP ─────┤──▶ "Is this bundle ready?"
    │                                     │
```

### Key Files

| Component | File |
|-----------|------|
| NTP timing and drift | `clockwork/js/lib/ntp_timing.js` |

## Component Overview

This is the web host. Natively, the socket or the NIF writes onto the same IN ring, and the engine's control thread drains the egress rings.

```
┌──────────────────────────────────────────────────┐
│                   Main Thread                    │
│  ┌──────────────┐    ┌──────────────┐            │
│  │  SuperSonic  │───▶│  OscChannel  │            │
│  │  (API entry) │    │ (transport)  │            │
│  └──────────────┘    └──────┬───────┘            │
│                             │                    │
│          every message, whatever its timetag     │
└─────────────────────────────┼────────────────────┘
                              │ SAB: IN ring buffer write
                              │ PM: postMessage
                              ▼
┌──────────────────────────────────────────────────┐
│                  AudioWorklet                    │
│  ┌────────────────────────────────────────────┐  │
│  │ IN ring buffer                             │  │
│  │ (PM: the worklet writes posted messages)   │  │
│  └─────────────────────┬──────────────────────┘  │
│                        ▼                         │
│  ┌────────────────────────────────────────────┐  │
│  │ OscIngress: due now → scsynth              │  │
│  │ timed bundles → scheduler, fired when due  │  │
│  │ (sample-accurate)                          │  │
│  └─────────────────────┬──────────────────────┘  │
│                        ▼                         │
│  ┌────────────────────────────────────────────┐  │
│  │     scsynth (audio engine)                 │  │
│  └─────────────────────┬──────────────────────┘  │
│                        │                         │
│              Reply OSC / Debug                   │
│                        ▼                         │
│  ┌────────────────────────────────────────────┐  │
│  │ OUT ring buffer                            │  │
│  └─────────────────────┬──────────────────────┘  │
│                        │ PM: read here and       │
│                        │ postMessage to main     │
└────────────────────────┼─────────────────────────┘
                         │ SAB: ring buffer
                         ▼
┌──────────────────────────┐
│   Reply Worker (SAB)     │
│                          │
│  Atomics.wait() on OUT   │
│  head — wakes on reply,  │
│  forwards to main thread │
│  via postMessage.        │
│                          │
│  (Main thread can't use  │
│   Atomics.wait itself —  │
│   it would freeze the UI)│
└──────────────────────────┘
```

## Message Flow

### Sending OSC to scsynth

1. **SuperSonic** receives OSC via `send()` or `sendOSC()`. On the web, `/b_alloc`, `/b_allocRead`, `/b_allocReadChannel` and `/b_allocFile` are rewritten to `/b_allocPtr` first: the client fetches and decodes the sample, and the engine is handed its place in memory.
2. **OscChannel** sends the bytes on without looking at their time:
   - SAB mode: written to the IN ring buffer
   - PM mode: postMessage to the AudioWorklet, which writes it onto the same IN ring
3. **The audio thread** drains the IN ring every block. A message, or a bundle that is due, goes to scsynth at once. A bundle with a later timetag is parked in clockwork's scheduler (`clockwork/src/scheduler/engine_schedule.h`) and handed to scsynth in the block it falls in, at its sample offset within that block. A bundle that arrives late plays at once. This is the same on every host.
4. **scsynth** processes the message at that sample.

There is no scheduling on the JavaScript side: however far ahead a bundle is timed, it goes onto the ring when it is sent.

### Receiving OSC from scsynth

On the web, scsynth writes replies (e.g. `/done`, `/n_go`) to the OUT ring buffer. This happens in both modes — the C++ code is identical regardless of transport. The difference is how those replies reach JavaScript.

**SAB mode — the reply worker (`osc_in_worker.js`)**

The main thread needs to hear replies, but `Atomics.wait()` is not allowed on the main thread (it would freeze the UI). So a dedicated Web Worker — the reply worker — sits in a blocking `Atomics.wait()` loop on the OUT buffer's head pointer. When scsynth writes a reply and advances the head, the reply worker wakes up, reads the message, and forwards it to the main thread via postMessage. This is the only reason the reply worker exists: it bridges SAB replies to the main thread's event loop.

**PM mode — no reply worker needed**

The AudioWorklet reads the OUT ring buffer directly and sends replies to the main thread via its MessagePort. There's no need for a separate worker because the AudioWorklet is already running on a dedicated thread.

**Both modes converge** at the SuperSonic event emitter on the main thread, which decodes the reply and emits `in` and `in:osc` events. On the web every reply rides the OUT ring: the arena also has an NRT-out ring, but the worklet has no second thread to write it. The only consumer is the reply path above (worker → main thread in SAB mode, worklet → main thread in PM mode). There is no per-channel reply delivery — every reply reaches clients through the main-thread event emitter.

Natively, replies written off the audio thread go on the NRT-out ring. The engine's control thread reads both rings, OUT first, and sends each reply to the client it answers, or to every subscriber of a broadcast.

**The OSC-out log**: the `out:osc` events show what was sent, read from the IN ring. The engine consumes that ring, so the log watches it through a tap: a read cursor of its own that takes nothing (`openTap`, `tapPoll`, `tapMissed` in `clockwork/js/lib/wasm_client.js`). Writers reserve space against the engine's cursor, not the tap's, so a log that falls behind is overwritten. The tap then resyncs to the newest traffic and counts what went past. In SAB mode the log worker (`osc_out_log_sab_worker.js`) runs the tap; in PM mode the worklet does, on its snapshot heartbeat.

### Debug Messages

Debug lines from the engine ride the egress as `/clockwork/debug` messages, on every host. On the web the client takes them out of the reply stream and emits them as `debug` events (`{ text, sequence, timestamp }`) instead of `in` events. The native server prints them to stderr.

## Key Files

| Component | File |
|-----------|------|
| SuperSonic API | `js/supersonic.js` |
| Buffer command rewriting (web) | `js/lib/osc_rewriter.js` |
| OscChannel | `clockwork/js/lib/osc_channel.js` |
| OscChannel (AudioWorklet-safe entry) | `clockwork/js/osc_channel.js` |
| Ring buffer read/write, taps | `clockwork/js/lib/wasm_client.js` over `clockwork/src/clockwork_client.cpp` |
| SAB transport | `clockwork/js/lib/transport/sab_transport.js` |
| PM transport | `clockwork/js/lib/transport/postmessage_transport.js` |
| Reply worker (SAB only) | `clockwork/js/workers/osc_in_worker.js` |
| OSC-out log worker (SAB only) | `clockwork/js/workers/osc_out_log_sab_worker.js` |
| AudioWorklet | `clockwork/js/workers/clockwork_audio_worklet.js` |
| NTP timing | `clockwork/js/lib/ntp_timing.js` |
| Engine entry (every host) | `clockwork/src/audio_processor.cpp` |
| Scheduler (every host) | `clockwork/src/scheduler/engine_schedule.h` (`EngineScheduler`) |
| scsynth guest | `dsp/scsynth/scsynth_dsp.cpp` |
| Memory layout | `clockwork/src/shared_memory.h`, `clockwork/src/clockwork_arena.h` |
| Region sizes | `clockwork/src/memory_profile.h` |

## Memory Layout

Every region is laid out once, before the engine runs: nothing is allocated for them later. The sizes are compile-time and vary by build (`clockwork/src/memory_profile.h`, overridden per product in `CMakeLists.txt`), so a reader finds each region through the arena's table of contents (`clockwork/src/clockwork_arena.h`) rather than by a fixed offset. SuperSonic's web build:

- **IN Ring Buffer**: 1 MB (JS -> scsynth; 768 KB in the native default)
- **OUT Ring Buffer**: 128 KB (replies written on the audio thread)
- **NRT-out Ring Buffer**: 64 KB (replies written on any other thread; natively the control thread's, unused on the web)
- **Control Region**: 56 B (ring cursors and flags; each egress ring has a read cursor apart from its tail, `out_read` at byte 44 and `nrt_out_read` at byte 48 — the region was 48 B up to v0.89.0)
- **Metrics Region**: 208 B (52 u32 counters)
- **Node Tree Mirror**: in the guest window, 98320 B by default: a 16 B header and 1024 entries of 96 B (synth hierarchy for visualization, includes UUIDs)

## Metrics Collection

Metrics are collected at all points in the system.

- **SAB mode**: written directly to shared metrics region, always current
- **PM mode**: the worklet posts a copy of the metrics region (and the node tree) on a heartbeat (default 150ms, configurable via `snapshotIntervalMs`); the transport counts the replies it receives itself

This means PM mode metrics can be up to one heartbeat interval stale.

## Multiple Writers

Multiple `OscChannel` instances can exist — one per worker, say — each sending straight to the audio thread: onto the IN ring in SAB mode, through its own MessagePort in PM mode. This supports scenarios like multiple instruments or control sources operating independently.

## Node ID Allocation

Every synth node in scsynth needs a unique integer ID. When you have multiple workers - a sequencer in one, a live keyboard in another, the main thread doing its own thing - those IDs must never collide. That's what `nextNodeId()` solves. It's available on both `SuperSonic` and `OscChannel`, so every context in the system can allocate IDs independently, guaranteed unique with no clashes.

How it works depends on the transport mode:

- **SAB mode**: a single `Atomics.add()` on a shared `Int32Array` in the SharedArrayBuffer. One atomic instruction, correct across all threads by hardware guarantee.
- **PM mode**: range-based allocation. The main thread hands out non-overlapping ranges to each worker (e.g. 1000-1999 to worker A, 2000-2999 to worker B). Workers increment locally within their range and pre-fetch the next range at the halfway point, so there's no round-trip pause under normal use.

IDs start at 1000. Below that: 0 is the root group, and 1-999 are left free for you to assign by hand.
