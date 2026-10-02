// Compile the Median length probe synthdef (upstream #7672, commit 8a483574d).
//
// Run with sclang from the supersonic repo root:
//
//     /Applications/SuperCollider.app/Contents/MacOS/sclang \
//         test/synthdefs/compile_median_synthdefs.scd
//
// Median's length is read once, in its constructor. A length below 1 left
// the insertion position at -1, which indexed the median window out of
// bounds; it is now clipped to 1, where Median passes its input through.
// The probe takes the median of a constant 0.5 and writes it to a control
// bus, so every length — including 0 and negatives — must read back 0.5.

var outputDir = PathName(thisProcess.nowExecutingPath).pathOnly;

SynthDef("median_length_probe", { |bus = 0|
    Out.kr(bus, Median.kr(\length.ir(3), DC.kr(0.5)));
}).writeDefFile(outputDir);

0.exit;
