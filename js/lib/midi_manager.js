// MidiManager — the web MIDI seam (main thread). Owns Web MIDI I/O and drives
// the shared Rust core (compiled to wasm) for all protocol logic: parsing
// inbound bytes to /midi/in/* OSC, decoding /midi/out/* to raw bytes, name
// normalisation, and clock-in BPM estimation. This is the web counterpart of the
// native MidiControl + Rust subsystem — the /midi/* OSC contract is identical.
//
// Web MIDI is main-thread only, so this runs alongside the SuperSonic JS, not in
// the AudioWorklet. Outbound sends use MIDIOutput.send(bytes, timestamp), so a
// timestamp keeps scheduled MIDI sample-tight via the browser's own scheduler.

import init, {
  midi_in_osc,
  midi_in_fields,
  midi_out_decode,
  normalize_name,
  WasmClockEstimator,
} from "../../dist/midi/supersonic_midi.js";

export class MidiManager {
  constructor() {
    this._access = null;
    this._inputs = new Map(); // normalized name -> MIDIInput
    this._outputs = new Map(); // normalized name -> MIDIOutput
    this._estimators = new Map(); // normalized name -> WasmClockEstimator
    this._onEvent = null; // (Uint8Array osc) => void  — /midi/in/* OSC packet
    this._onMessage = null; // (Array [kind, port, ...ints]) => void  — structured
    this._onPorts = null; // ({ins, outs}) => void
    this._onTempo = null; // (port, bpm) => void  — clock-in
    this._lastPortsKey = null; // last emitted port list, to suppress no-op pushes
  }

  // Load the wasm core and acquire Web MIDI. Resolves once ports are enumerated.
  async init() {
    await init();
    if (!navigator.requestMIDIAccess) throw new Error("Web MIDI API unavailable");
    this._access = await navigator.requestMIDIAccess({ sysex: true });
    this._access.onstatechange = () => this._refresh();
    this._refresh();
    return this;
  }

  onEvent(cb) { this._onEvent = cb; }
  // Structured inbound events: cb receives a flat [kind, port, ...ints] array
  // (e.g. ["note_on", "kbd", 1, 60, 100]) with no OSC encode/decode round-trip.
  // Takes precedence over onEvent when both are set.
  onMessage(cb) { this._onMessage = cb; }
  onPorts(cb) { this._onPorts = cb; }
  onTempo(cb) { this._onTempo = cb; }

  _refresh() {
    this._inputs.clear();
    this._outputs.clear();
    const ins = [];
    const outs = [];
    for (const input of this._access.inputs.values()) {
      const name = normalize_name(input.name || input.id);
      this._inputs.set(name, input);
      ins.push(name);
      input.onmidimessage = (e) => this._onInput(name, e);
    }
    for (const output of this._access.outputs.values()) {
      const name = normalize_name(output.name || output.id);
      this._outputs.set(name, output);
      outs.push(name);
    }
    // Web MIDI fires statechange for transient/duplicate transitions; only
    // notify when the port list actually changed, mirroring the native
    // ss_midi_refresh "broadcast only on change" behaviour.
    const key = JSON.stringify([ins, outs]);
    if (key === this._lastPortsKey) return;
    this._lastPortsKey = key;
    if (this._onPorts) this._onPorts({ ins, outs });
  }

  _onInput(port, event) {
    const bytes = event.data;
    // Clock pulses feed the estimator (→ tempo), never surfaced as events.
    if (bytes.length === 1 && bytes[0] === 0xf8) {
      let est = this._estimators.get(port);
      if (!est) {
        est = new WasmClockEstimator();
        this._estimators.set(port, est);
      }
      const bpm = est.update(event.timeStamp * 1000.0); // ms → µs
      if (bpm != null && this._onTempo) this._onTempo(port, bpm);
      return;
    }
    // Prefer the structured fast path; fall back to OSC bytes for consumers
    // (e.g. the native-shaped engine ingress) that want the wire form.
    if (this._onMessage) {
      const fields = midi_in_fields(port, bytes);
      if (fields) this._onMessage(fields);
      return;
    }
    const osc = midi_in_osc(port, bytes);
    if (osc && this._onEvent) this._onEvent(osc);
  }

  // Send a /midi/out/* OSC packet to hardware. `timestampMs` (a
  // DOMHighResTimeStamp) schedules it; omit for immediate.
  sendOut(oscBytes, timestampMs) {
    const packed = midi_out_decode(oscBytes);
    if (!packed) return;
    const portLen = packed[0];
    const port = new TextDecoder().decode(packed.subarray(1, 1 + portLen));
    const raw = packed.subarray(1 + portLen);
    if (port === "*") {
      for (const out of this._outputs.values()) out.send(raw, timestampMs);
    } else {
      const out = this._outputs.get(port);
      if (out) out.send(raw, timestampMs);
    }
  }
}
