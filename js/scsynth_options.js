// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2025-2026 Sam Aaron
/*
 * scsynth_options.js — the engine's options, as the web client takes them.
 *
 * THE GUEST'S OPTIONS ARE LISTED ONCE, in dsp/scsynth/scsynth_options.h.
 * lib/scsynth_options_schema.js is generated from that header
 * (scripts/gen-scsynth-options.mjs), and everything here — the defaults, the
 * validation, the block the engine reads — derives from it. Nothing in this
 * file names a number of the engine's.
 *
 * The block itself is TEXT, one `name=value` per line, which the guest
 * parses by name: there is no slot to keep in step with a C header, and an
 * option the engine does not know refuses the boot with a message naming
 * it, rather than being silently ignored.
 *
 * A few words are the WEB HOST'S OWN and never reach the guest: which
 * channels to open on the audio graph, the render quantum, and three that
 * scsynth's command line has and a worklet cannot use. They are validated
 * here and consumed here (supersonic.js), and they are listed below so a
 * caller who knows scsynth's options finds them under the names they expect.
 */
import { scsynthOptionSchema } from "./lib/scsynth_options_schema.js";

const guestDefaults = Object.fromEntries(scsynthOptionSchema.map((o) => [o.name, o.default]));

/*
 * The web host's own options, with their defaults. Validated and consumed by
 * the client; not part of the block the guest reads.
 */
const hostDefaults = {
  /** Input channels the client reads from the device (0 disables input). */
  numInputBusChannels: 2,
  /** Output channels the client opens on the audio graph (1–128). */
  numOutputBusChannels: 2,
  /** The render quantum. FIXED at 128 by the Web Audio API. */
  bufLength: 128,
  /** scsynth's real-time flag. Always false here: the worklet drives the engine. */
  realTime: false,
  /** scsynth's memory locking. Not applicable in a browser. */
  memoryLocking: false,
  /** Preferred sample rate; 0 uses the AudioContext's own. */
  preferredSampleRate: 0,
};

const defaultScsynthOptions = Object.freeze({ ...guestDefaults, ...hostDefaults });

export { defaultScsynthOptions };
export default defaultScsynthOptions;

/** The names of the guest's options, in the schema's order. */
export const guestOptionNames = Object.freeze(scsynthOptionSchema.map((o) => o.name));

/*
 * Refuse what the engine would refuse, before a worklet is spun up for it:
 * the ranges are the schema's, and the message names the option so a caller
 * can find it. Defaults are merged in before this runs, so every field is
 * present and each is checked unconditionally — an undefined here means a
 * caller passed one explicitly as undefined, which is worth refusing.
 */
export function validateScsynthOptions(opts) {
  const numeric = (name, min, max) => {
    const v = opts[name];
    if (typeof v !== "number" || !Number.isFinite(v)) {
      throw new Error(`scsynthOptions.${name} must be a finite number, got: ${v}`);
    }
    if (v < min) throw new Error(`scsynthOptions.${name} must be >= ${min}, got: ${v}`);
    if (max !== undefined && v > max) {
      throw new Error(`scsynthOptions.${name} must be <= ${max}, got: ${v}`);
    }
  };
  for (const o of scsynthOptionSchema) {
    numeric(o.name, o.min, o.max);
    if (!Number.isInteger(opts[o.name])) {
      throw new Error(`scsynthOptions.${o.name} must be an integer, got: ${opts[o.name]}`);
    }
  }

  // The host's own.
  numeric("numInputBusChannels", 0);
  numeric("numOutputBusChannels", 1, 128);
  numeric("preferredSampleRate", 0, 384000);
  // 128 is not scsynth's rule but AudioWorklet's: it renders 128 frames and
  // nothing else. It is checked here because this is where the field with
  // that name lives.
  if (opts.bufLength !== 128) {
    throw new Error(
      `scsynthOptions.bufLength must be 128 (WebAudio API constraint), got: ${opts.bufLength}`);
  }
  for (const name of ["realTime", "memoryLocking"]) {
    if (typeof opts[name] !== "boolean") {
      throw new Error(`scsynthOptions.${name} must be a boolean, got: ${typeof opts[name]}`);
    }
  }
  if (opts.preferredSampleRate !== 0 && opts.preferredSampleRate < 8000) {
    throw new Error(
      `scsynthOptions.preferredSampleRate must be 0 (auto) or >= 8000, got: ${opts.preferredSampleRate}`);
  }
}

/*
 * The block the engine reads: `name=value` lines, NUL-terminated, only the
 * guest's own options. Clockwork copies these bytes into the region it
 * reserves (DspConfig::guest_config) and reads none of them.
 */
export function encodeScsynthOptions(o) {
  let text = "";
  for (const opt of scsynthOptionSchema) {
    const v = o[opt.name] ?? opt.default;
    text += `${opt.name}=${v}\n`;
  }
  const bytes = new TextEncoder().encode(text + "\0");
  // An ArrayBuffer, which is what the worklet copies from.
  return bytes.buffer.slice(bytes.byteOffset, bytes.byteOffset + bytes.byteLength);
}
