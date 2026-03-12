var outputDir = "/home/sam/Development/supersonic/dist/synthdefs";

// SynthDef for testing UnitCmdDemo unit commands.
// UnitCmdDemo outputs its 'value' field (set via /u_cmd setValue).
// The UGen index within this synthdef is 1 (Out.kr is 0, UnitCmdDemo is 1).
SynthDef(\u_cmd_test, { |bus|
    Out.kr(bus, UnitCmdDemo.kr);
}).writeDefFile(outputDir);

// Simple synthdef for use as completion message target.
// Writes a constant value to a control bus.
SynthDef(\number, { |bus|
    Out.kr(bus, 1.0);
}).writeDefFile(outputDir);

"Done writing demo synthdefs to:".postln;
outputDir.postln;

0.exit;
