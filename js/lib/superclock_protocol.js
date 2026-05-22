// SPDX-License-Identifier: MIT OR GPL-3.0-or-later
// Copyright (c) 2025 Sam Aaron

/**
 * SuperClock cross-thread protocol — field offsets in the SuperClockState
 * SAB region and PM-mode worklet message types. Imported by both
 * js/lib/superclock.js and js/workers/scsynth_audio_worklet.js so the
 * protocol cannot drift between them.
 *
 * Mirrors src/shared_memory.h SuperClockState. Doubles are stored as
 * raw IEEE 754 bit-patterns in 64-bit atomics (BigInt64Array + Atomics
 * on the JS side; std::atomic<uint64_t> on the C++ side).
 *
 * Layout (32 bytes):
 *   [0-7]   bpm                (uint64 bit-pattern of double)
 *   [8-15]  beat_origin_ntp    (uint64 bit-pattern of double)
 *   [16-23] is_playing_at_ntp  (uint64 bit-pattern of double)
 *   [24-27] is_playing         (uint32: 0 or 1)
 *   [28-31] padding
 */

// BigInt64Array indices (8-byte stride).
export const SC_BPM_I64                = 0;
export const SC_BEAT_ORIGIN_NTP_I64    = 1;
export const SC_IS_PLAYING_AT_NTP_I64  = 2;

// Int32Array index (4-byte stride) — byte offset 24 / 4.
export const SC_IS_PLAYING_I32         = 6;

// PM-mode worklet message types. The main thread posts these and the
// worklet's onmessage handler dispatches on them.
export const SuperClockMessageType = Object.freeze({
  SET_SESSION_BPM:              'setSessionBpm',
  SET_SESSION_IS_PLAYING:       'setSessionIsPlaying',
  SET_SESSION_BEAT_ORIGIN_NTP:  'setSessionBeatOriginNtp',
});

// Reusable scratch buffer for double ↔ BigInt64 conversion. Single-thread
// JS access; no contention. Hoisted to module scope so setBpm / getBpm /
// etc. don't allocate per call.
const _scratchBuf = new ArrayBuffer(8);
const _scratchF64 = new Float64Array(_scratchBuf);
const _scratchI64 = new BigInt64Array(_scratchBuf);

export function doubleToBits(v) {
  _scratchF64[0] = v;
  return _scratchI64[0];
}

export function bitsToDouble(bits) {
  _scratchI64[0] = bits;
  return _scratchF64[0];
}
