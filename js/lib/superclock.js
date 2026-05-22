// SPDX-License-Identifier: MIT OR GPL-3.0-or-later
// Copyright (c) 2025 Sam Aaron

/**
 * SuperClock — single JS-side authority for session state and time.
 * Mirrors the C++ SuperClock API (src/SuperClock.h).
 *
 * Owns:
 *   - Session state: BPM, transport, beat origin. SAB-mode writes the
 *     SuperClockState region directly via BigInt64 atomics; PM-mode
 *     posts to the worklet which writes the same region.
 *   - Time: ntp_start_time / drift / clock-offset via a composed
 *     private NTPTiming helper.
 */

import { NTPTiming } from './ntp_timing.js';
import {
  SC_BPM_I64,
  SC_BEAT_ORIGIN_NTP_I64,
  SC_IS_PLAYING_AT_NTP_I64,
  SC_IS_PLAYING_I32,
  SuperClockMessageType,
  doubleToBits, bitsToDouble,
} from './superclock_protocol.js';

export class SuperClock {
  #mode;
  #workletPort;
  #audioContext;

  // SAB views over the SuperClockState region (same bytes, two strides).
  #sabBigInt;  // BigInt64Array — for the three double fields
  #sabInt32;   // Int32Array    — for the is_playing field

  // PM-mode local copies. In SAB mode these are also kept up-to-date so
  // getters have a fallback before initSharedViews has run.
  #localBpm = 120.0;
  #localBeatOriginNtp = 0.0;
  #localIsPlaying = false;
  #localIsPlayingAtNtp = 0.0;

  #ntp;

  /**
   * @param {Object} options
   * @param {'sab'|'postMessage'} options.mode
   * @param {AudioContext} options.audioContext
   * @param {MessagePort} [options.workletPort] — required in PM mode
   */
  constructor(options = {}) {
    this.#mode = options.mode || 'sab';
    this.#workletPort = options.workletPort || null;
    this.#audioContext = options.audioContext || null;
    this.#ntp = new NTPTiming({
      mode: this.#mode,
      audioContext: options.audioContext,
      workletPort: this.#workletPort,
    });
  }

  /**
   * Initialize SAB views for session state and time.
   * @param {SharedArrayBuffer} sharedBuffer
   * @param {number} ringBufferBase
   * @param {Object} bufferConstants — must include SUPERCLOCK_STATE_START / _SIZE
   */
  initSharedViews(sharedBuffer, ringBufferBase, bufferConstants) {
    if (this.#mode === 'sab') {
      const base = ringBufferBase + bufferConstants.SUPERCLOCK_STATE_START;
      const size = bufferConstants.SUPERCLOCK_STATE_SIZE;
      this.#sabBigInt = new BigInt64Array(sharedBuffer, base, size / 8);
      this.#sabInt32  = new Int32Array(sharedBuffer, base, size / 4);
    }
    this.#ntp.initSharedViews(sharedBuffer, ringBufferBase, bufferConstants);
  }

  /** @param {MessagePort} port */
  setWorkletPort(port) { this.#workletPort = port; }

  updateAudioContext(audioContext) {
    this.#audioContext = audioContext;
    this.#ntp.updateAudioContext(audioContext);
  }

  // ── Time / drift surface (delegates to private NTPTiming) ──────────────

  async initialize() { await this.#ntp.initialize(); }
  resync()           { this.#ntp.resync(); }
  startDriftTimer()  { this.#ntp.startDriftTimer(); }
  stopDriftTimer()   { this.#ntp.stopDriftTimer(); }
  updateDriftOffset(){ this.#ntp.updateDriftOffset(); }
  getDriftOffset()   { return this.#ntp.getDriftOffset(); }
  getNTPStartTime()  { return this.#ntp.getNTPStartTime(); }
  getClockOffset()   { return this.#ntp.getClockOffset(); }
  setClockOffset(s)  { this.#ntp.setClockOffset(s); }
  reset()            { this.#ntp.reset(); }

  // ── Session mutators ───────────────────────────────────────────────────

  /**
   * @param {number} bpm
   * @param {number} [atNtpSeconds=0] — honoured by a Link backing
   */
  setBpm(bpm, atNtpSeconds = 0) {
    this.#localBpm = bpm;
    if (this.#sabBigInt) {
      Atomics.store(this.#sabBigInt, SC_BPM_I64, doubleToBits(bpm));
    } else if (this.#workletPort) {
      this.#workletPort.postMessage({
        type: SuperClockMessageType.SET_SESSION_BPM,
        bpm, atNtpSeconds,
      });
    }
  }

  /**
   * @param {boolean} playing
   * @param {number} [atNtpSeconds=0]
   */
  setIsPlaying(playing, atNtpSeconds = 0) {
    this.#localIsPlaying = !!playing;
    this.#localIsPlayingAtNtp = atNtpSeconds;
    if (this.#sabBigInt) {
      Atomics.store(this.#sabBigInt, SC_IS_PLAYING_AT_NTP_I64, doubleToBits(atNtpSeconds));
      Atomics.store(this.#sabInt32,  SC_IS_PLAYING_I32, playing ? 1 : 0);
    } else if (this.#workletPort) {
      this.#workletPort.postMessage({
        type: SuperClockMessageType.SET_SESSION_IS_PLAYING,
        isPlaying: this.#localIsPlaying, atNtpSeconds,
      });
    }
  }

  /** @param {boolean} enabled */
  setLinkEnabled(enabled) {}

  /**
   * @param {number} beat
   * @param {number} atNtpSeconds
   * @param {number} quantum
   */
  requestBeatAtTime(beat, atNtpSeconds, quantum) {
    const bpm = this.getBpm();
    const newOrigin = atNtpSeconds - (beat * 60.0) / bpm;
    this.#localBeatOriginNtp = newOrigin;
    if (this.#sabBigInt) {
      Atomics.store(this.#sabBigInt, SC_BEAT_ORIGIN_NTP_I64, doubleToBits(newOrigin));
    } else if (this.#workletPort) {
      this.#workletPort.postMessage({
        type: SuperClockMessageType.SET_SESSION_BEAT_ORIGIN_NTP,
        beatOriginNtp: newOrigin,
      });
    }
  }

  forceBeatAtTime(beat, atNtpSeconds, quantum) {
    this.requestBeatAtTime(beat, atNtpSeconds, quantum);
  }

  // ── Session getters ────────────────────────────────────────────────────

  getBpm() {
    if (this.#sabBigInt) return bitsToDouble(Atomics.load(this.#sabBigInt, SC_BPM_I64));
    return this.#localBpm;
  }

  isPlaying() {
    if (this.#sabInt32) return Atomics.load(this.#sabInt32, SC_IS_PLAYING_I32) !== 0;
    return this.#localIsPlaying;
  }

  getBeatOriginNtp() {
    if (this.#sabBigInt) return bitsToDouble(Atomics.load(this.#sabBigInt, SC_BEAT_ORIGIN_NTP_I64));
    return this.#localBeatOriginNtp;
  }

  getIsPlayingAtNtp() {
    if (this.#sabBigInt) return bitsToDouble(Atomics.load(this.#sabBigInt, SC_IS_PLAYING_AT_NTP_I64));
    return this.#localIsPlayingAtNtp;
  }

  isLinkEnabled() { return false; }
  numPeers()      { return 0; }

  /**
   * Current NTP time as seen by the audio thread — `AudioContext.currentTime`
   * + the engine's NTP-start anchor + drift + global offset.
   *
   * Use this to schedule events relative to "now in audio time":
   *
   *   sonic.sendOSC(osc.encodeBundle(sonic.superClock.now() + 0.05, packets));
   *
   * That bundle fires 50 ms of *audio time* from now. The audio thread is
   * reading the same clock, so the two sides always agree — no skew if
   * AudioContext is throttled, no need to know the NTP-start formula.
   */
  now() {
    if (!this.#audioContext) return 0;
    return this.nowAt(this.#audioContext.currentTime);
  }

  /**
   * Compute audio-thread NTP for a specific `AudioContext.currentTime`.
   * `now()` calls this with the live `currentTime`; advanced callers can
   * pass a specific value (e.g. from `audioContext.getOutputTimestamp()`
   * for tight sample-aligned scheduling).
   */
  nowAt(audioCurrentTime) {
    // getDriftOffset / getClockOffset both return milliseconds; divide
    // by 1000 to get seconds. (C++ reads raw microseconds from the SAB
    // and divides by 1e6, getting the same seconds value with µs
    // precision — JS-side precision is ms because NTPTiming rounds.)
    return audioCurrentTime
         + this.getNTPStartTime()
         + this.getDriftOffset() / 1000
         + this.getClockOffset() / 1000;
  }

  /**
   * Current NTP time as seen by the system wall clock — `performance.now()`
   * + `performance.timeOrigin`, converted to NTP. Independent of the audio
   * clock; useful when matching against external wall-clock events (e.g.
   * network MIDI, NTP sources). For scheduling engine events, prefer
   * {@link now} so the audio thread and the scheduler agree.
   */
  wallNow() {
    return (performance.timeOrigin + performance.now()) / 1000 + 2208988800;
  }

  // ── Beat math ──────────────────────────────────────────────────────────
  // Reads bpm and beat_origin individually — no multi-field coherence
  // guarantee. Today's only callers are app-thread (single-writer JS).

  beatAtTime(ntpSeconds, quantum) {
    return (ntpSeconds - this.getBeatOriginNtp()) * this.getBpm() / 60.0;
  }

  phaseAtTime(ntpSeconds, quantum) {
    const beat = this.beatAtTime(ntpSeconds, quantum);
    let phase = beat % quantum;
    if (phase < 0) phase += quantum;
    return phase;
  }

  timeAtBeat(beat, quantum) {
    return this.getBeatOriginNtp() + beat * 60.0 / this.getBpm();
  }
}
