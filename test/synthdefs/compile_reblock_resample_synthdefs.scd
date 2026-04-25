// Generate synthdef test fixtures that exercise Reblock() and Resample() (PR #7402).
// Requires sclang built from supercollider commit a27ec6c01 or later.
// Run with: sclang -a --include-path /path/to/SCClassLibrary compile_reblock_resample_synthdefs.scd
//
// Strategy: each probe synthdef writes BlockSize.ir and SampleRate.ir to two
// consecutive control buses (bus, bus+1). Reading them back via /c_get tells
// us the *actual* per-Graph rate values, which is the only way to prove the
// Reblock/Resample feature is wired up correctly.
//
// Expected values (server defaults: blockSize=128, sampleRate=48000):
//   baseline                       → BlockSize=128, SampleRate=48000
//   Reblock(32)                    → BlockSize=32,  SampleRate=48000
//   Reblock(64)                    → BlockSize=64,  SampleRate=48000
//   Resample(2)                    → BlockSize=128, SampleRate=96000
//   Resample(4)                    → BlockSize=128, SampleRate=192000
//   Reblock(32) + Resample(2)      → BlockSize=32,  SampleRate=96000
//   Reblock(blockSize=N), default  → BlockSize=N,   SampleRate=48000
//   Resample(factor=N), default    → BlockSize=128, SampleRate=N*48000

var outDir = thisProcess.nowExecutingPath.dirname +/+ "reblock_resample";
File.mkdir(outDir);

// Baseline: no Reblock or Resample.
SynthDef(\rr_probe_baseline, { |bus=0|
    Out.kr(bus, [BlockSize.ir, SampleRate.ir]);
}).writeDefFile(outDir);

// Reblock with constant block size of 32.
SynthDef(\rr_probe_reblock32, { |bus=0|
    Reblock(32);
    Out.kr(bus, [BlockSize.ir, SampleRate.ir]);
}).writeDefFile(outDir);

// Reblock with constant block size of 64.
SynthDef(\rr_probe_reblock64, { |bus=0|
    Reblock(64);
    Out.kr(bus, [BlockSize.ir, SampleRate.ir]);
}).writeDefFile(outDir);

// Resample 2× upsample.
SynthDef(\rr_probe_resample2, { |bus=0|
    Resample(2);
    Out.kr(bus, [BlockSize.ir, SampleRate.ir]);
}).writeDefFile(outDir);

// Resample 4× upsample.
SynthDef(\rr_probe_resample4, { |bus=0|
    Resample(4);
    Out.kr(bus, [BlockSize.ir, SampleRate.ir]);
}).writeDefFile(outDir);

// Combined: Reblock(32) + Resample(2).
SynthDef(\rr_probe_reblock_resample, { |bus=0|
    Reblock(32);
    Resample(2);
    Out.kr(bus, [BlockSize.ir, SampleRate.ir]);
}).writeDefFile(outDir);

// Reblock with control-driven block size (default 16).
SynthDef(\rr_probe_reblock_ctrl, { |bus=0, blockSize=16|
    Reblock(blockSize);
    Out.kr(bus, [BlockSize.ir, SampleRate.ir]);
}).writeDefFile(outDir);

// Resample with control-driven factor (default 2.0).
SynthDef(\rr_probe_resample_ctrl, { |bus=0, factor=2|
    Resample(factor);
    Out.kr(bus, [BlockSize.ir, SampleRate.ir]);
}).writeDefFile(outDir);

// Audio-output variant (still useful for end-to-end smoke test).
// 440 Hz sine via Reblock(32) + Resample(2) — proves audio still flows.
SynthDef(\rr_audio_reblock_resample, {
    Reblock(32);
    Resample(2);
    Out.ar(0, SinOsc.ar(440) * 0.2);
}).writeDefFile(outDir);

"Done! Wrote 9 reblock/resample fixtures to: %\n".postf(outDir);

0.exit;
