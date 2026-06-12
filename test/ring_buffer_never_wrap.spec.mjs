// Frames never wrap the ring boundary: a frame that doesn't fit before the
// end is preceded by a PADDING_MAGIC marker (magic word + zeros to the
// boundary) and restarts at offset 0, so payloads are always contiguous and
// readers copy them with plain linear reads. These drive the JS writer and
// reader (ring_buffer_core.js) directly over a tiny buffer where the boundary
// is easy to hit, and pin the reader's corruption policy (resync-to-head,
// stop) against rings that violate the invariant.
import { test, expect } from '@playwright/test';
import {
  writeMessageToBuffer,
  canWriteMessage,
  readMessagesFromBuffer,
  copyWrappedPayload,
} from '../js/lib/ring_buffer_core.js';

const MAGIC = 0xdeadbeef;
const PADDING = 0xbaddcafe;  // PADDING_MAGIC from src/shared_memory.h
const HEADER = 16;

function makeRing(bufferSize) {
  const buffer = new ArrayBuffer(bufferSize);
  return {
    bufferSize,
    uint8View: new Uint8Array(buffer),
    dataView: new DataView(buffer),
  };
}

// Write one message at `head` (caller guarantees fit), then drain from `tail`
// and return { payloads, newTail, head: newHead }.
function writeAndDrain(ring, { head, tail, payload, sequence = 1 }) {
  const aligned = (HEADER + payload.length + 3) & ~3;
  expect(canWriteMessage(head, tail, ring.bufferSize, aligned)).toBe(true);
  const newHead = writeMessageToBuffer({
    uint8View: ring.uint8View, dataView: ring.dataView,
    bufferStart: 0, bufferSize: ring.bufferSize, head,
    payload: Uint8Array.from(payload), sequence,
    messageMagic: MAGIC, paddingMagic: PADDING, headerSize: HEADER,
  });
  const payloads = [];
  const { newTail } = readMessagesFromBuffer({
    uint8View: ring.uint8View, dataView: ring.dataView,
    bufferStart: 0, bufferSize: ring.bufferSize,
    head: newHead, tail,
    messageMagic: MAGIC, paddingMagic: PADDING, headerSize: HEADER,
    onMessage: (payloadOffset, payloadLength) => {
      payloads.push(Array.from(
        ring.uint8View.subarray(payloadOffset, payloadOffset + payloadLength)));
    },
  });
  return { payloads, newTail, head: newHead };
}

test('a frame that misses the boundary is padded and lands contiguously at 0', () => {
  const ring = makeRing(64);
  // head 44, frame 24 (16 header + 8 payload): only 20 bytes to the end, so
  // the writer must emit a padding marker at 44 and write the frame at 0.
  const payload = [10, 20, 30, 40, 50, 60, 70, 80];
  const { payloads, newTail, head } = writeAndDrain(ring, { head: 44, tail: 44, payload });

  expect(ring.dataView.getUint32(44, true)).toBe(PADDING);     // marker emitted
  expect(Array.from(ring.uint8View.subarray(48, 64))).toEqual( // zeros to the end
    new Array(16).fill(0));
  expect(head).toBe(24);                                       // frame at 0..24
  expect(payloads).toEqual([payload]);                         // linear read correct
  expect(newTail).toBe(head);                                  // fully consumed
});

test('a frame ending exactly at the boundary needs no padding; head wraps to 0', () => {
  const ring = makeRing(64);
  // head 32, frame 32: ends exactly at 64 → head returns to 0, no marker.
  const payload = Array.from({ length: 16 }, (_, i) => i + 1);
  const { payloads, newTail, head } = writeAndDrain(ring, { head: 32, tail: 32, payload });
  expect(head).toBe(0);
  expect(payloads).toEqual([payload]);
  expect(newTail).toBe(0);
});

test('canWriteMessage rejects a frame with free space but no contiguous room', () => {
  // head 56, tail 40 on a 64-byte ring: 47 bytes free in total, but only 8 to
  // the end and 39 at the front — a 44-byte frame must be rejected.
  expect(canWriteMessage(56, 40, 64, 44)).toBe(false);
  // The same frame fits once the tail clears: tail 48 → 47 at the front.
  expect(canWriteMessage(56, 48, 64, 44)).toBe(true);
});

test('reader treats a padding marker at offset 0 as corruption, not a spin', () => {
  const ring = makeRing(64);
  ring.dataView.setUint32(0, PADDING, true);  // can never be legitimately written
  let corrupt = 0;
  const { newTail, messagesRead } = readMessagesFromBuffer({
    uint8View: ring.uint8View, dataView: ring.dataView,
    bufferStart: 0, bufferSize: 64, head: 32, tail: 0,
    messageMagic: MAGIC, paddingMagic: PADDING, headerSize: HEADER,
    onMessage: () => {},
    onCorruption: () => { corrupt++; },
  });
  expect(messagesRead).toBe(0);
  expect(corrupt).toBe(1);
  expect(newTail).toBe(32);  // resynced to head — terminates, drops the region
});

test('reader rejects a frame whose footprint crosses the ring boundary', () => {
  const ring = makeRing(64);
  // Hand-craft a legacy/hostile wrap-split frame at 44 claiming 32 bytes
  // (44 + 32 > 64): must be corruption, not a read past the ring's end.
  ring.dataView.setUint32(44, MAGIC, true);
  ring.dataView.setUint32(48, 32, true);   // length
  ring.dataView.setUint32(52, 0, true);    // sequence
  ring.dataView.setUint32(56, 0, true);    // sourceId
  let corrupt = 0;
  const { newTail, messagesRead } = readMessagesFromBuffer({
    uint8View: ring.uint8View, dataView: ring.dataView,
    bufferStart: 0, bufferSize: 64, head: 12, tail: 44,
    messageMagic: MAGIC, paddingMagic: PADDING, headerSize: HEADER,
    onMessage: () => {},
    onCorruption: () => { corrupt++; },
  });
  expect(messagesRead).toBe(0);
  expect(corrupt).toBe(1);
  expect(newTail).toBe(12);  // resynced to head
});

test('reader rejects a frame claiming more bytes than are published', () => {
  const ring = makeRing(64);
  // Valid magic at 0 with a bit-flipped length of 48 while only 24 bytes are
  // published (head 24): waiting would stall the lane forever.
  ring.dataView.setUint32(0, MAGIC, true);
  ring.dataView.setUint32(4, 48, true);
  let corrupt = 0;
  const { newTail } = readMessagesFromBuffer({
    uint8View: ring.uint8View, dataView: ring.dataView,
    bufferStart: 0, bufferSize: 64, head: 24, tail: 0,
    messageMagic: MAGIC, paddingMagic: PADDING, headerSize: HEADER,
    onMessage: () => {},
    onCorruption: () => { corrupt++; },
  });
  expect(corrupt).toBe(1);
  expect(newTail).toBe(24);
});

test('copyWrappedPayload remains a correct linear copy with destOffset (worklet pool case)', () => {
  const dest = new Uint8Array(8).fill(0xff);
  const src = new Uint8Array(8);
  for (let i = 0; i < 8; i++) src[i] = i + 1;
  copyWrappedPayload(src, 0, 8, 0, 4, dest, 2);
  expect(Array.from(dest)).toEqual([0xff, 0xff, 1, 2, 3, 4, 0xff, 0xff]);
});
