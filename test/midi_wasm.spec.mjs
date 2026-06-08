/**
 * WASM MIDI: the shared Rust core (parse/encode/estimator) and the JS
 * MidiManager running in a real browser. Web MIDI is mocked so the round-trip is
 * deterministic in headless Chromium (real Web MIDI needs a device + permission).
 */
import { test, expect } from "@playwright/test";

test.describe("WASM MIDI", () => {
  test("Rust core parse/encode + clock estimator run in the browser", async ({ page }) => {
    await page.goto("/test/midi_wasm.html");
    await page.waitForFunction(() => window.midiWasmReady === true);

    const r = await page.evaluate(async () => {
      const w = window.wasmMidi;
      await w.initWasm();

      // inbound: raw note-on → /midi/in/note_on OSC
      const inOsc = w.midi_in_osc("kbd", new Uint8Array([0x90, 60, 100]));
      const inAddr = new TextDecoder().decode(inOsc.subarray(0, inOsc.indexOf(0)));

      // outbound: /midi/out/note_on "out" ch1 note64 vel99 → raw MIDI bytes
      const outMsg = w.encodeOsc("/midi/out/note_on", [
        { t: "s", v: "out" }, { t: "i", v: 1 }, { t: "i", v: 64 }, { t: "i", v: 99 },
      ]);
      const packed = w.midi_out_decode(outMsg);
      const portLen = packed[0];
      const port = new TextDecoder().decode(packed.subarray(1, 1 + portLen));
      const raw = Array.from(packed.subarray(1 + portLen));

      // clock-in estimator: feed 120 BPM pulses
      const est = new w.WasmClockEstimator();
      const ivUs = (60 / 120 / 24) * 1e6;
      let t = 0, bpm = null;
      for (let i = 0; i < 100; i++) { bpm = est.update(t); t += ivUs; }

      return { inAddr, port, raw, bpm };
    });

    expect(r.inAddr).toBe("/midi/in/note_on");
    expect(r.port).toBe("out");
    expect(r.raw).toEqual([0x90, 64, 99]); // channel 1 → status 0x90
    expect(Math.abs(r.bpm - 120)).toBeLessThan(1);
  });

  test("MidiManager round-trips through (mocked) Web MIDI", async ({ page }) => {
    await page.goto("/test/midi_wasm.html");
    await page.waitForFunction(() => window.midiWasmReady === true);

    const r = await page.evaluate(async () => {
      // Mock Web MIDI: one input, one output named "out".
      const sent = [];
      const input = { id: "in1", name: "Test In", onmidimessage: null };
      const output = { id: "out1", name: "out", send: (d) => sent.push(Array.from(d)) };
      const access = {
        inputs: new Map([["in1", input]]),
        outputs: new Map([["out1", output]]),
        onstatechange: null,
      };
      Object.defineProperty(navigator, "requestMIDIAccess", {
        value: async () => access,
        configurable: true,
      });

      const m = new window.MidiManager();
      const events = [];
      m.onEvent((osc) => events.push(new TextDecoder().decode(osc.subarray(0, osc.indexOf(0)))));
      await m.init();

      // device → engine: inbound note-on becomes a /midi/in/note_on event
      input.onmidimessage({ data: new Uint8Array([0x90, 60, 100]), timeStamp: performance.now() });

      // engine → device: /midi/out/note_on reaches the output as raw bytes
      const outMsg = window.wasmMidi.encodeOsc("/midi/out/note_on", [
        { t: "s", v: "out" }, { t: "i", v: 1 }, { t: "i", v: 64 }, { t: "i", v: 99 },
      ]);
      m.sendOut(outMsg);

      return { events, sent };
    });

    expect(r.events).toContain("/midi/in/note_on");
    expect(r.sent).toContainEqual([0x90, 64, 99]);
  });
});
