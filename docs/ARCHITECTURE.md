# SuperSonic Architecture

SuperSonic ports SuperCollider's scsynth audio engine to run in a WebAssembly AudioWorklet. This document explains the architecture and the design decisions behind it.

## Core Challenges

### 1. AudioWorklet Constraints
The WASM scsynth runs inside an AudioWorklet with strict requirements:
- No thread spawning
- No malloc (memory must be pre-allocated)
- No I/O
- No main() entry point
- No automatic C++ initializer calls

The original scsynth was multi-threaded with separate threads for I/O and audio graph calculations. We bypass this by using SharedArrayBuffer memory accessible to both WASM and JS.

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

OSC bundles carry NTP timestamps indicating when they should execute. All timestamps throughout SuperSonic are NTP-based (seconds since 1900-01-01). However, the AudioWorklet has no access to `performance.now()` or the system clock - it only receives the audio clock timestamp (`currentTime`) passed into `process()`.

The AudioWorklet must translate between audio clock and NTP to know when to dispatch scheduled bundles to scsynth.

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
currentNTP = audioContextTime + ntpStartTime + driftOffset
```

### Drift Management

The audio clock and system clock drift relative to each other (hardware crystals aren't perfect). At typical 100ppm drift, clocks can diverge ~0.1ms per second.

We measure and correct for drift:

1. **Main thread** periodically (every 1000ms) compares expected vs actual `contextTime`:
   ```
   expectedContextTime = currentNTP - ntpStartTime
   driftMs = (expectedContextTime - actualContextTime) * 1000
   ```

2. **Drift offset** is written to shared memory (SAB mode) or sent via postMessage (PM mode)

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
| NTP timing | `js/lib/ntp_timing.js` |
| Timing utilities | `js/lib/timing_utils.js` |
| Timing constants | `js/timing_constants.js` |

## Component Overview

```
┌─────────────────────────────────────────────────────────────────────────┐
│                           Main Thread                                   │
│  ┌──────────────┐    ┌─────────────┐                                    │
│  │  SuperSonic  │───▶│  OSCChannel │──┬─────────────────────────────┐   │
│  │  (API entry) │    │  (router)   │  │                             │   │
│  └──────────────┘    └─────────────┘  │                             │   │
│                                       │                             │   │
│         Immediate/near-future OSC     │    Far-future bundles       │   │
│                   │                   │         (>500ms)            │   │
└───────────────────┼───────────────────┼─────────────────────────────┘   │
                    │                   │                                 │
                    ▼                   ▼                                 │
┌──────────────────────────┐  ┌─────────────────────┐                     │
│      AudioWorklet        │  │   Prescheduler      │                     │
│  ┌────────────────────┐  │  │      Worker         │                     │
│  │ SAB: ring buffer   │  │  │                     │                     │
│  │ PM: postMessage    │  │  │  Parks bundles,     │                     │
│  │       queue        │  │  │  dispatches when    │◀────────────────────┘
│  └─────────┬──────────┘  │  │  ready              │
│            │             │  └──────────┬──────────┘
│            ▼             │             │
│  ┌────────────────────┐  │             │ SAB: ring buffer write
│  │ WASM Scheduler     │◀─┼─────────────┘ PM: postMessage
│  │ (sample-accurate)  │  │
│  └─────────┬──────────┘  │
│            │             │
│            ▼             │
│  ┌────────────────────┐  │
│  │     scsynth        │  │
│  │   (audio engine)   │  │
│  └─────────┬──────────┘  │
│            │             │
│     Reply OSC / Debug    │
│            │             │
│            ▼             │
│  ┌────────────────────┐  │
│  │ OUT/DEBUG buffers  │  │
│  │ SAB: direct read   │  │
│  │ PM: postMessage    │  │
│  └────────────────────┘  │
└──────────────────────────┘
```

## Message Flow

### Sending OSC to scsynth

1. **SuperSonic** receives OSC via `send()` or `sendOSC()`
2. **OSCChannel** classifies the message:
   - **Immediate/non-bundle**: bypass direct to AudioWorklet
   - **Near-future bundle** (<=500ms): bypass direct to AudioWorklet
   - **Late bundle** (past timestamp): bypass direct (already late)
   - **Far-future bundle** (>500ms): route to Prescheduler
3. **Direct route** (bypass):
   - SAB mode: write to IN ring buffer
   - PM mode: postMessage to AudioWorklet
4. **Prescheduler route**: stores bundle until ~500ms before timestamp, then dispatches via its own direct connection
5. **AudioWorklet** receives message:
   - SAB mode: reads from ring buffer
   - PM mode: receives via postMessage, queues internally
6. **WASM Scheduler** receives message with sample-accurate timestamp
7. **scsynth** processes at the exact sample

### Receiving OSC from scsynth

1. **scsynth** generates reply (e.g., `/done`, `/n_go`)
2. Reply written to OUT ring buffer (both modes - keeps WASM code identical)
3. **SAB mode**: dedicated worker reads buffer via Atomics.wait, emits events
4. **PM mode**: AudioWorklet reads buffer, sends via postMessage
5. **SuperSonic** emits `onReply` event

**Lapping detection**: In SAB mode, the log worker maintains its own read tail (`IN_LOG_TAIL`) independent of the C++ consumer's tail. If the writer wraps the ring buffer and overtakes the log reader, the log worker detects the invalid magic number at its read position, resyncs to head, and skips the corrupted batch rather than reading corrupt data.

### Debug Messages

Same pattern as OSC replies but via DEBUG buffer and `onDebug` event.

## Key Files

| Component | File |
|-----------|------|
| SuperSonic API | `js/supersonic.js` |
| OSC routing | `js/lib/osc_channel.js` |
| OscChannel (AudioWorklet-safe) | `js/osc_channel.js` |
| Ring buffer read/write | `js/lib/ring_buffer_core.js` |
| SAB transport | `js/lib/transport/sab_transport.js` |
| PM transport | `js/lib/transport/postmessage_transport.js` |
| Prescheduler | `js/workers/osc_out_prescheduler_worker.js` |
| AudioWorklet | `js/workers/scsynth_audio_worklet.js` |
| NTP timing | `js/lib/ntp_timing.js` |
| WASM entry | `src/audio_processor.cpp` |
| WASM scheduler | `src/scheduler/BundleScheduler.h` |
| Memory layout | `src/shared_memory.h` |

## Memory Layout

Pre-allocated in WASM memory (no runtime allocation):

- **IN Ring Buffer**: 768KB (JS -> scsynth)
- **OUT Ring Buffer**: 128KB (scsynth -> JS replies)
- **DEBUG Buffer**: 64KB (debug messages)
- **Control Region**: 48B (atomic pointers/flags)
- **Metrics Region**: 168B (performance counters)
- **Node Tree Mirror**: ~57KB (synth hierarchy for visualization)

## Metrics Collection

Metrics are collected at all points in the system.

- **SAB mode**: written directly to shared metrics region, always current
- **PM mode**: each worker keeps local tallies, sends snapshot deltas on heartbeat (default 150ms, configurable via `snapshotIntervalMs`)

This means PM mode metrics can be up to one heartbeat interval stale.

## Multiple Writers

Multiple `OSCChannel` instances can exist, each with its own direct line to the AudioWorklet. This supports scenarios like multiple instruments or control sources operating independently.
