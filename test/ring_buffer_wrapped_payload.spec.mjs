// The IN ring's JS writer splits a message's payload across the buffer boundary
// (writeMessageToBuffer), but the log readers copied the payload with a LINEAR
// `uint8View[payloadOffset + i]` loop — which runs past the buffer end (into the
// adjacent OUT region) for any payload that wraps, producing garbage log output.
//
// copyWrappedPayload centralises a wrap-aware copy so a payload spanning the ring
// boundary is reassembled correctly. These drive it directly over a tiny buffer
// where a wrap is easy to force.
import { test, expect } from '@playwright/test';
import {
  writeMessageToBuffer,
  readMessagesFromBuffer,
  copyWrappedPayload,
} from '../js/lib/ring_buffer_core.js';

const MAGIC = 0xdeadbeef;
const PADDING = 0xdeadfeed;
const HEADER = 16;

// Write one message at `head`, then read it back and copy its payload via
// copyWrappedPayload. Returns the reassembled payload bytes.
function roundTrip({ bufferSize, head, payload }) {
  const buffer = new ArrayBuffer(bufferSize);
  const uint8View = new Uint8Array(buffer);
  const dataView = new DataView(buffer);

  writeMessageToBuffer({
    uint8View, dataView, bufferStart: 0, bufferSize, head,
    payload: Uint8Array.from(payload), sequence: 1, messageMagic: MAGIC, headerSize: HEADER,
  });

  let got = null;
  readMessagesFromBuffer({
    uint8View, dataView, bufferStart: 0, bufferSize, head: (head + ((HEADER + payload.length + 3) & ~3)) % bufferSize,
    tail: head, messageMagic: MAGIC, paddingMagic: PADDING, headerSize: HEADER,
    onMessage: (payloadOffset, payloadLength) => {
      const dest = new Uint8Array(payloadLength);
      copyWrappedPayload(uint8View, 0, bufferSize, payloadOffset, payloadLength, dest, 0);
      got = Array.from(dest);
    },
  });
  return got;
}

test('copyWrappedPayload reassembles a payload that wraps the ring boundary', () => {
  // bufferSize 64, head 44: header (16B) fits at 44..59, payload (8B) splits —
  // 4 bytes at 60..63, 4 bytes wrapped to 0..3. A linear read would walk indices
  // 64..67 (past the buffer) and lose the wrapped half.
  const payload = [10, 20, 30, 40, 50, 60, 70, 80];
  expect(roundTrip({ bufferSize: 64, head: 44, payload })).toEqual(payload);
});

test('copyWrappedPayload is correct for a contiguous (non-wrapping) payload', () => {
  // head 0: everything fits with room to spare, no wrap.
  const payload = [1, 2, 3, 4, 5, 6, 7, 8];
  expect(roundTrip({ bufferSize: 64, head: 0, payload })).toEqual(payload);
});

test('copyWrappedPayload honours destOffset (worklet pool case)', () => {
  const dest = new Uint8Array(8).fill(0xff);
  const src = new Uint8Array(8);
  for (let i = 0; i < 8; i++) src[i] = i + 1;
  // No wrap here: offset 0, length 8, into dest at offset 2.
  copyWrappedPayload(src, 0, 8, 0, 4, dest, 2);
  expect(Array.from(dest)).toEqual([0xff, 0xff, 1, 2, 3, 4, 0xff, 0xff]);
});
