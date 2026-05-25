// Test-only SynthDef for OSC array-tag parity. Has an arrayed control
// `freqs = #[110, 220, 330, 440]` (four slots starting at the named
// control). Each slot is piped to a distinct audio bus via K2A.ar so
// the test can read them back and verify the n_setn-style application.
//
// Run with sclang:
//
//   /Applications/SuperCollider.app/Contents/MacOS/sclang \
//       test/synthdefs/compile_array_control_synthdefs.scd

var outputDir = thisProcess.argv[0] ?? {
    PathName(thisProcess.nowExecutingPath).pathOnly
};

SynthDef(\array_control_probe, { |out = 0, freqs = #[110, 220, 330, 440]|
    Out.ar(out,     K2A.ar(freqs[0]));
    Out.ar(out + 1, K2A.ar(freqs[1]));
    Out.ar(out + 2, K2A.ar(freqs[2]));
    Out.ar(out + 3, K2A.ar(freqs[3]));
}).writeDefFile(outputDir);

"Wrote array_control_probe.scsyndef to:".postln;
outputDir.postln;

0.exit;
