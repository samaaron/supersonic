# Scope streams + the SuperClock sample clock

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
(the capture taps written by `AudioOut2`) already implements the right
protocol: a fixed-layout SPSC ring with a monotonic 64-bit `write_position`,
lossless catch-up reads and gap detection.

Scope slots now use that protocol, which gives every consumer deterministic
windows and latency alignment without per-widget reconstruction.

## The sample-clock region

A new 32-byte region appended at the end of the arena (nothing shifts; the
segment stays self-describing via `shm_segment_header`):

```
[0..3]   u32 seq        seqlock: odd = writer mid-update
[4..7]   u32 sample_rate
[8..15]  u64 engine_frames   engine sample position at block start
[16..23] f64 dac_ntp         NTP seconds when engine_frames hits the DAC
                             (= block render NTP + device output latency)
[24..27] u32 output_latency_frames   (observability)
[28..31] u32 reserved
```

Owned by SuperClock (`bindSampleClockToShm` at engine init;
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
independent memory_profile knob, `SHM_SCOPE_RING_FRAMES`, default 16384 ≈
340ms @ 48k; embedded profiles set it small):

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

`ScopeOut2_Ctor` claims and activates its slot through `fGetScopeBuffer`
(which retains only claim/release-ownership semantics — see the contract
note in `SC_InterfaceTable.h`), then `ScopeOut2_next` appends every block
directly via a `shm_scope_stream_writer`, anchoring on `g_engine_frames`.
The first write sets `base_engine_frames`; later writes heal forward cursor
gaps (paused node groups). The old period/accumulation machinery is gone and
`fPushScopeBuffer` is a no-op. Slot-owner guarding (a superseded unit's late
dtor must not stomp a re-claimed slot) is unchanged.

## Readers

- `shm_scope_stream_reader` (shm_scope_stream.hpp): `valid/channels/
  capacity_frames`, `write_position()`, `base_engine_frames()`, and
  `copy_window(end_cursor, frames, out, &used_channels)` — zero-fills what
  the ring no longer holds and stays `SHM_SCOPE_READ_MARGIN_FRAMES` clear of
  the writer.
- `sample_clock_view` from `server_shared_memory_client::get_sample_clock()`:
  seqlock-consistent `{engine_frames, dac_ntp, sample_rate,
  output_latency_frames}` plus `audible_end(reader)` — the canonical window
  end for every scope consumer.
- GUI widgets: each repaint tick calls `audible_end` and copies the display
  window. Poll rate affects only frame rate, never correctness.
- js/supersonic.js `getScope` reads the ring by cursor (layout keeps coming
  from `get_buffer_layout()`); the read margin formula must stay in step
  with the C++ constant.

## Consumers migrated

1. Inline live-loop scopes (`LiveLoopScopeWidget`)
2. Card + jukebox scopes (`ScopeSampler`)
3. Main scope dock (api `AudioProcessor` slot-0 consumption; keeps its
   `ProcessedAudio` delivery shape)

## Compatibility

- Segment layout is self-describing; both sides compile from one header.
- WASM: the JS reader is ported, but no worklet driver publishes the sample
  clock yet — web streams fall back to raw-cursor windows (unaligned) and
  the paused-group heal is inert there.
- The GUI↔engine `/supersonic/info` outputLatencySamples field stays: the
  code-flash delay uses it. Scope alignment no longer does.
- Known limitation: the engine sample counter is per-driver and resets on
  device restart, so streams that survive a warm swap / pause-resume keep a
  stale anchor until re-claimed. An epoch on the sample clock is the planned
  fix.
