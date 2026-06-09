// Unit test for AudioCapture.read() bounds. Drives read() directly over a
// synthetic shm_audio slot (no real audio / no browser) so the ring-overflow
// path is deterministic.
//
// read() used `frames = write_position` (a monotonic counter) as the length of
// the Float32Array view over the fixed-capacity `data` ring. A capture that runs
// longer than capacity_frames therefore asks for a view past the data region —
// a RangeError (or a read into neighbouring SAB memory). read() must clamp to
// capacity_frames: the ring only physically holds that many frames.
import { test, expect } from '@playwright/test';
import { AudioCapture } from '../js/lib/audio_capture.js';

// Header layout (matches shm_audio_buffer.hpp / audio_capture.js):
//   [0] enabled  [1] sample_rate  [2] channels  [3] capacity_frames
//   [4] wpos_low [5] wpos_high    [6..7] padding   then float data
const HEADER_BYTES = 32;
const IDX = { ENABLED: 0, SAMPLE_RATE: 1, CHANNELS: 2, CAPACITY: 3, WPOS_LOW: 4, WPOS_HIGH: 5 };

// Build an AudioCapture backed by a SAB sized to EXACTLY header + capacity*channels
// floats — no slack — so an over-length read is forced to throw rather than
// silently reading adjacent memory.
function makeCapture({ capacity, channels, wpos, sampleRate = 48000 }) {
  const dataFloats = capacity * channels;
  const sab = new SharedArrayBuffer(HEADER_BYTES + dataFloats * 4);
  const h = new Uint32Array(sab, 0, HEADER_BYTES / 4);
  h[IDX.ENABLED] = 0;
  h[IDX.SAMPLE_RATE] = sampleRate;
  h[IDX.CHANNELS] = channels;
  h[IDX.CAPACITY] = capacity;
  h[IDX.WPOS_LOW] = wpos;
  h[IDX.WPOS_HIGH] = 0;

  // Fill the data region with recognisable interleaved values: frame f channel c
  // = f*10 + c.
  const data = new Float32Array(sab, HEADER_BYTES, dataFloats);
  for (let f = 0; f < capacity; f++)
    for (let c = 0; c < channels; c++) data[f * channels + c] = f * 10 + c;

  const cap = new AudioCapture({
    sharedBuffer: sab,
    ringBufferBase: 0,
    bufferConstants: { SHM_AUDIO_START: 0, SHM_AUDIO_HEADER_SIZE: HEADER_BYTES },
  });
  return cap;
}

test('read() clamps frames to capacity when write_position has overflowed the ring', () => {
  // 8-frame ring, but 100 frames "written" (monotonic counter kept climbing).
  const cap = makeCapture({ capacity: 8, channels: 2, wpos: 100 });
  const out = cap.read();
  // Must not read past the 8-frame data region.
  expect(out.frames).toBe(8);
  expect(out.left.length).toBe(8);
  expect(out.right.length).toBe(8);
});

test('read() returns all frames unchanged when below capacity', () => {
  // 4 of 8 frames written — the normal case must be untouched by the clamp.
  const cap = makeCapture({ capacity: 8, channels: 2, wpos: 4 });
  const out = cap.read();
  expect(out.frames).toBe(4);
  expect(out.channels).toBe(2);
  // frame f channel c == f*10 + c
  expect(Array.from(out.left)).toEqual([0, 10, 20, 30]);
  expect(Array.from(out.right)).toEqual([1, 11, 21, 31]);
});
