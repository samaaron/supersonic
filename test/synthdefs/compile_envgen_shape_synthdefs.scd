// Compile the EnvGen signed-shape probe synthdefs (sonic-pi#169).
//
// Run with sclang from the supersonic repo root:
//
//     /Applications/SuperCollider.app/Contents/MacOS/sclang \
//         test/synthdefs/compile_envgen_shape_synthdefs.scd
//
// Each probe writes its envelope value straight to the output bus so a
// capture can assert the trajectory. The \sqr and \cub warps historically
// NaN'd on negative levels, so the -1 -> 1 probes are the regression
// cases and the 0 -> 1 probe guards legacy behaviour.

var outputDir = PathName(thisProcess.nowExecutingPath).pathOnly;

SynthDef("envgen_sqr_probe", { |out = 0|
    var env = EnvGen.kr(Env([-1, 1], [0.25], \sqr));
    Out.ar(out, K2A.ar(env) ! 2);
}).writeDefFile(outputDir);

SynthDef("envgen_cub_probe", { |out = 0|
    var env = EnvGen.kr(Env([-1, 1], [0.25], \cub));
    Out.ar(out, K2A.ar(env) ! 2);
}).writeDefFile(outputDir);

SynthDef("envgen_sqr_pos_probe", { |out = 0|
    var env = EnvGen.kr(Env([0, 1], [0.25], \sqr));
    Out.ar(out, K2A.ar(env) ! 2);
}).writeDefFile(outputDir);

0.exit;
