// Compile the two-RandID probe synthdef (upstream #6159, commit 91cc120).
//
// Run with sclang from the supersonic repo root:
//
//     /Applications/SuperCollider.app/Contents/MacOS/sclang \
//         test/synthdefs/compile_randid_synthdefs.scd
//
// RandID chooses which of the World's random generators a synth draws from.
// It used to switch only when its input changed, so with two RandIDs in one
// SynthDef the synth stayed on the last one's generator after the first block.
// It now switches on every control block. The probe has two branches, each
// switching to its own generator, seeding it the same and drawing white noise:
// channel 0 is the absolute difference of the two draws, which must be
// silent; channel 1 is one draw, which must not be, so a synth that renders
// nothing cannot pass. The class library change in the same commit is only
// whitespace, so any sclang compiles this the same.

var outputDir = PathName(thisProcess.nowExecutingPath).pathOnly;

SynthDef("randid_two_ids_probe", {
    var x, y;
    #x, y = Array.fill(2, { |i|
        RandID.kr(i);
        RandSeed.kr(1, 36163);
        WhiteNoise.ar
    });
    Out.ar(0, [x absdif: y, x]);
}).writeDefFile(outputDir);

0.exit;
