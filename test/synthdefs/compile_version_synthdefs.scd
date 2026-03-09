// Generate synthdef test fixtures in v1, v2, and v3 formats.
// Run with: sclang -a --include-path /path/to/SCClassLibrary compile_version_synthdefs.scd /path/to/outdir

var outDir = thisProcess.argv[0] ? (thisProcess.nowExecutingPath.dirname +/+ "versions");
var graphFunc, graphFunc2, stream;

File.mkdir(outDir);

// Simple synthdef: sine oscillator with controllable freq and amp
graphFunc = { |out=0, freq=440, amp=0.5|
    Out.ar(out, SinOsc.ar(freq) * amp);
};

// Multi-control synthdef to exercise param specs more thoroughly
graphFunc2 = { |out=0, freq=440, amp=0.5, pan=0, gate=1|
    var sig = SinOsc.ar(freq) * amp;
    var env = EnvGen.kr(Env.asr(0.01, 1, 0.1), gate, doneAction: 2);
    Out.ar(out, Pan2.ar(sig * env, pan));
};

// --- Version 1 (SynthDefOld) ---
stream = CollStream.on(Int8Array.new(256));
[SynthDefOld(\test_simple, graphFunc)].writeDefOld(stream);
File.use(outDir +/+ "test_simple_v1.scsyndef", "wb", { |f| f.write(stream.collection) });

stream = CollStream.on(Int8Array.new(512));
[SynthDefOld(\test_multi, graphFunc2)].writeDefOld(stream);
File.use(outDir +/+ "test_multi_v1.scsyndef", "wb", { |f| f.write(stream.collection) });

// --- Version 2 ---
stream = CollStream.on(Int8Array.new(256));
[SynthDef(\test_simple, graphFunc)].writeDef(stream, 2);
File.use(outDir +/+ "test_simple_v2.scsyndef", "wb", { |f| f.write(stream.collection) });

stream = CollStream.on(Int8Array.new(512));
[SynthDef(\test_multi, graphFunc2)].writeDef(stream, 2);
File.use(outDir +/+ "test_multi_v2.scsyndef", "wb", { |f| f.write(stream.collection) });

// --- Version 3 ---
stream = CollStream.on(Int8Array.new(256));
[SynthDef(\test_simple, graphFunc)].writeDef(stream, 3);
File.use(outDir +/+ "test_simple_v3.scsyndef", "wb", { |f| f.write(stream.collection) });

stream = CollStream.on(Int8Array.new(512));
[SynthDef(\test_multi, graphFunc2)].writeDef(stream, 3);
File.use(outDir +/+ "test_multi_v3.scsyndef", "wb", { |f| f.write(stream.collection) });

"Done! Wrote 6 synthdef fixtures to: %\n".postf(outDir);

0.exit;
