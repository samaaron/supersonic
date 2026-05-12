// Compile the supersonic-audio-out synthdef.
//
// Run with sclang from the supersonic repo root, with the AudioOut2
// class shim on the include path:
//
//     sclang --include-path test/synthdefs/sc-classes \
//            test/synthdefs/compile_audio_out.scd
//
// Reads `bus` (stereo) and writes it into shm_audio_buffer slot `slot`.
// Defaults wire master output (bus 0) into slot 0; both parameters can
// be overridden at /s_new time to tap arbitrary buses into slots 1..N-1.
//
// The shipped startCapture/stopCapture API does not use this synthdef:
// slot 0 is driven directly from the audio thread's post-block hook in
// audio_processor.cpp. This synthdef exists for /s_new'ing AudioOut2
// against any (bus, slot) pair from user code.

var outputDir = thisProcess.argv[0] ?? {
    PathName(thisProcess.nowExecutingPath).pathOnly +/+
        "../../packages/supersonic-scsynth-synthdefs/synthdefs"
};

SynthDef(\supersonic_audio_out, { |slot = 0, bus = 0|
    AudioOut2.ar(slot, In.ar(bus, 2));
}).name_("supersonic-audio-out").writeDefFile(outputDir);

"Wrote supersonic-audio-out.scsyndef to:".postln;
outputDir.postln;

0.exit;
