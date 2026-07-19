// Compile the EnvGen exponential-shape probe synthdefs (sonic-pi#881).
//
// Run with sclang from the supersonic repo root:
//
//     /Applications/SuperCollider.app/Contents/MacOS/sclang \
//         test/synthdefs/compile_envgen_exp_synthdefs.scd
//
// The \exp warp is undefined at zero: upstream computes
// grow = pow(end/start, 1/n) then level *= grow, so a zero start sticks
// the whole segment at silence and jumps at the boundary (the click in
// sonic-pi#881). The zero-endpoint probe is the regression case, the
// non-zero probe guards legacy behaviour.

var outputDir = PathName(thisProcess.nowExecutingPath).pathOnly;

SynthDef("envgen_exp_probe", { |out = 0|
    var env = EnvGen.kr(Env([0, 1, 0], [0.25, 0.25], \exp));
    Out.ar(out, K2A.ar(env) ! 2);
}).writeDefFile(outputDir);

SynthDef("envgen_exp_pos_probe", { |out = 0|
    var env = EnvGen.kr(Env([0.001, 1], [0.25], \exp));
    Out.ar(out, K2A.ar(env) ! 2);
}).writeDefFile(outputDir);

0.exit;
