# Scope streams + the ClockworkClock sample clock

Status: shipped. Replaced the triple-buffered
scope slots with the same lossless cursor-ring protocol the audio-capture taps
already use, plus a single shared **sample clock** that maps engine sample position
to wall-clock DAC time.

## Why

The triple-buffered scope slot was lossy: the writer overwrites unread
regions, the reader only ever sees the newest publish, and nothing relates a
sample to the moment it becomes audible. Every consumer that wanted a stream
(the Sonic Pi inline live-loop scopes, card scopes, main scope) needed
GUI-side workarounds: publish-race polling faster than the writer,
arrival-time latency guessing, per-widget reassembly rings. `shm_audio_buffer`
(the capture taps clockwork writes at the device edge, on every host) already
implements the right protocol: a fixed-layout SPSC ring with a monotonic
64-bit `write_position`, lossless catch-up reads and gap detection.

Scope slots now use that protocol, which gives every consumer deterministic
windows and latency alignment without per-widget reconstruction.

## The sample-clock region

A 32-byte region in the clockwork block of the arena, after the clock state
(`SAMPLE_CLOCK_START` in `clockwork/src/shared_memory.h`). A reader finds it
through the arena's table of contents, as the entry
`CLOCKWORK_ARENA_SAMPLE_CLOCK` (`clockwork/src/clockwork_arena.h`):

```
[0..3]   u32 seq        seqlock: odd = writer mid-update
[4..7]   u32 sample_rate
[8..15]  u64 engine_frames   engine sample position at block start
[16..23] f64 dac_ntp         NTP seconds when engine_frames hits the DAC
                             (= block render NTP + device output latency)
[24..27] u32 output_latency_frames   (observability)
[28..31] u32 reserved
```

Owned by ClockworkClock (`bindSampleClockToShm` at engine init;
`publishSampleClock` once per hardware callback from each audio driver, with
`advanceEngineFrames` per rendered block keeping stream anchors exact),
seqlock-ordered. Any reader can convert "now" (its own `system_clock` read,
same host) into an engine sample position:

```
visible_frames = engine_frames + (now_ntp - dac_ntp) * sample_rate
```

i.e. the newest sample the listener has heard. Used by the scope streams;
also usable for recording markers and visual sync.

## Scope stream slots

The scope region's slots change from `header + 3 × region` to a
`shm_audio_buffer`-shaped ring (own struct so scope ring size is an
independent memory_profile knob, `SHM_SCOPE_RING_FRAMES`). Its size per host
(`clockwork/src/memory_profile.h`):

- **Native:** 131072 frames, ≈ 2.7 s at 48 kHz (16384 up to v0.89.0). A
  consumer draws the window being heard, which sits the device's output
  latency behind the writer, and a wireless output can report two seconds
  of it.
- **Web:** 16384 frames, ≈ 340 ms at 48 kHz.
- **Embedded profiles** (ESP32-S3, Teensy 4.1): 512 frames.

The ring must hold the output latency, the longest display window (250 ms)
and the reader's margin (`shm_scope_ring_covers` in
`clockwork/src/shm_scope_stream.hpp`). The native engine checks this at every
device start and logs a warning when the device's output latency is more than
the ring covers: scopes would show nothing on that device.

```
[0..3]   u32 state (atomic; 0=free, 1=active)
[4..7]   u32 channels
[8..11]  u32 capacity_frames
[12..15] u32 reserved
[16..23] u64 write_position (atomic; frames since activation)
[24..31] u64 base_engine_frames (engine sample position of the first write)
[32..]   float data[capacity * channels]  interleaved, wraps
```

`base_engine_frames` ties a slot-local cursor to the global sample clock:
`slot_visible = visible_frames - base_engine_frames`, clamped to
`[write_position - capacity + margin, write_position]`.

## Writer: ScopeOut2

Clockwork owns the slots and writes them; scsynth reaches them only through
`DspHost` (`clockwork/src/dsp_api.h`). `ScopeOut2_Ctor` claims and activates
its slot through `fGetScopeBuffer` (which retains only claim/release-ownership
semantics — see the contract note in `SC_InterfaceTable.h`), which becomes
`DspHost::scope_open`. Then `ScopeOut2_next` hands every block to
`supersonic_scope_write` (`dsp/scsynth/scsynth_dsp.cpp`), which calls
`DspHost::scope_write`. On clockwork's side (`dsp_host_scope_write` in
`clockwork/src/audio_processor.cpp`) a `shm_scope_stream_writer` appends the
block, anchoring on `g_engine_frames`. The first write sets
`base_engine_frames`; later writes heal forward cursor gaps (paused node
groups). The old period/accumulation machinery is gone and `fPushScopeBuffer`
is a no-op. Slot-owner guarding (a superseded unit's late dtor must not stomp
a re-claimed slot) is unchanged: a release is owner-guarded by the handle's
address.

## Readers

- `shm_scope_stream_reader` (shm_scope_stream.hpp): `valid/channels/
  capacity_frames`, `write_position()`, `base_engine_frames()`, and
  `copy_window(end_cursor, frames, out, &used_channels)` — zero-fills what
  the ring no longer holds and stays `SHM_SCOPE_READ_MARGIN_FRAMES` clear of
  the writer.
- `sample_clock_view` from `shm_segment_client::get_sample_clock()`
  (`clockwork/src/shm_segment.hpp`): seqlock-consistent `{engine_frames,
  dac_ntp, sample_rate, output_latency_frames}` plus `audible_end(reader)` —
  the canonical window end for every scope consumer. A C client gets the
  same through `clockwork_client_scope_audible_end`
  (`clockwork/src/clockwork_client.h`).
- GUI widgets: each repaint tick calls `audible_end` and copies the display
  window. Poll rate affects only frame rate, never correctness.
- `getScope` in `clockwork/js/clockwork.js` reads the ring by cursor, SAB mode
  only, and returns the window ending at the write cursor, not at the audible
  sample. Its layout comes from the arena's table of contents; the read
  margin formula must stay in step with the C++ constant.

## Consumers migrated

1. Inline live-loop scopes (`LiveLoopScopeWidget`)
2. Card + jukebox scopes (`ScopeSampler`)
3. Main scope dock (api `AudioProcessor` slot-0 consumption; keeps its
   `ProcessedAudio` delivery shape)

## Compatibility

- Segment layout is self-describing; both sides compile from one header.
- WASM: the worklet publishes the sample clock once per rendered block, from
  the time the host has written the engine's NTP start, with an output
  latency of 0 (it knows no device latency, so a frame is stamped as
  rendered). `g_engine_frames` advances with it, so stream anchors and the
  paused-group heal work there too. `clockwork/js/lib/sample_clock.js` reads
  the region from JavaScript; `getScope` does not use it yet.
- The GUI↔engine `/clockwork/info` outputLatencySamples field stays: the
  code-flash delay uses it. Scope alignment no longer does.
- Known limitation: the engine sample counter is per-driver and resets on
  device restart, so streams that survive a warm swap / pause-resume keep a
  stale anchor until re-claimed. An epoch on the sample clock is the planned
  fix.
