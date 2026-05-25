// Compile test-only synthdefs used by the LinkUGen test suite.
// Output to test/synthdefs/; EngineFixture::loadSynthDef falls back to
// this dir when a name isn't in the shipped synthdef package.
//
// Run with sclang, pointing it at the LinkUGen class shim:
//
//     /Applications/SuperCollider.app/Contents/MacOS/sclang \
//         --include-path test/synthdefs/sc-classes \
//         test/synthdefs/compile_link_ugen_test_synthdefs.scd
//
// link_tempo_probe: outputs the current Link tempo (CPS) on `out` as
//   an audio-rate signal (K2A so the test fixture can sample via the
//   audio bus pool). Returns -1 when Link is not active.
//
// link_phase_probe: outputs the current Link phase within `quantum`
//   on `out`. Returns -1 when Link is not active.
//
// link_jump_trigger: triggers LinkJump on `trig > 0` and emits silence
//   on `out`. Used to drive jump-then-observe-phase tests.

var outputDir = thisProcess.argv[0] ?? {
    PathName(thisProcess.nowExecutingPath).pathOnly
};

SynthDef(\link_tempo_probe, { |out = 0|
    // LinkTempo.kr(set_flag, tempo_cps). set_flag <= 0 means read-only.
    Out.ar(out, K2A.ar(LinkTempo.kr(0, 0)));
}).writeDefFile(outputDir);

SynthDef(\link_phase_probe, { |out = 0, quantum = 4|
    Out.ar(out, K2A.ar(LinkPhase.kr(quantum)));
}).writeDefFile(outputDir);

SynthDef(\link_jump_trigger, { |out = 0, trig = 0, beat = 0, quantum = 4, hard = 1|
    LinkJump.kr(trig, beat, quantum, hard);
    Out.ar(out, DC.ar(0));
}).writeDefFile(outputDir);

"Wrote link_tempo_probe.scsyndef, link_phase_probe.scsyndef, link_jump_trigger.scsyndef to:".postln;
outputDir.postln;

0.exit;
