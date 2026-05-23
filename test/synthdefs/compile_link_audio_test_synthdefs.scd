// Compile test-only synthdefs used by the Link Audio integration suite.
// Output to test/synthdefs/; EngineFixture::loadSynthDef falls back to
// this dir when a name isn't in the shipped synthdef package. Test-only
// definitions don't pollute the npm artifact.
//
// Run with sclang from the SuperCollider app bundle:
//
//     /Applications/SuperCollider.app/Contents/MacOS/sclang \
//         test/synthdefs/compile_link_audio_test_synthdefs.scd
//
// stereo_passthrough: reads 2-channel from in_bus, writes those two
// channels unchanged to out and out+1. Used by the LinkAudio
// integration tests to verify the full data path — including channel
// distinction — flows through scsynth's In.ar / Out.ar.

var outputDir = thisProcess.argv[0] ?? {
    PathName(thisProcess.nowExecutingPath).pathOnly
};

SynthDef(\stereo_passthrough, { |out = 0, in_bus = 0|
    Out.ar(out, In.ar(in_bus, 2));
}).writeDefFile(outputDir);

"Wrote stereo_passthrough.scsyndef to:".postln;
outputDir.postln;

0.exit;
