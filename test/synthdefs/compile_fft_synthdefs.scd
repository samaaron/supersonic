var outputDir = "/home/sam/Development/supersonic/dist/synthdefs";

SynthDef(\fft_passthrough, { |out=0, in_bus=0| var sig, chain; sig = In.ar(in_bus, 1); chain = FFT(LocalBuf(2048), sig); Out.ar(out, IFFT(chain).dup); }).writeDefFile(outputDir);

SynthDef(\fft_brickwall, { |out=0, in_bus=0, wipe=0.5| var sig, chain; sig = In.ar(in_bus, 1); chain = FFT(LocalBuf(2048), sig); chain = PV_BrickWall(chain, wipe); Out.ar(out, IFFT(chain).dup); }).writeDefFile(outputDir);

SynthDef(\fft_magfreeze, { |out=0, in_bus=0, freeze=0| var sig, chain; sig = In.ar(in_bus, 1); chain = FFT(LocalBuf(2048), sig); chain = PV_MagFreeze(chain, freeze); Out.ar(out, IFFT(chain).dup); }).writeDefFile(outputDir);

SynthDef(\fft_test_sine, { |out=0, freq=440, amp=0.5| Out.ar(out, SinOsc.ar(freq, 0, amp)); }).writeDefFile(outputDir);

SynthDef(\fft_size_512, { |out=0, in_bus=0| var sig, chain; sig = In.ar(in_bus, 1); chain = FFT(LocalBuf(512), sig); Out.ar(out, IFFT(chain).dup); }).writeDefFile(outputDir);

SynthDef(\fft_size_1024, { |out=0, in_bus=0| var sig, chain; sig = In.ar(in_bus, 1); chain = FFT(LocalBuf(1024), sig); Out.ar(out, IFFT(chain).dup); }).writeDefFile(outputDir);

SynthDef(\fft_size_4096, { |out=0, in_bus=0| var sig, chain; sig = In.ar(in_bus, 1); chain = FFT(LocalBuf(4096), sig); Out.ar(out, IFFT(chain).dup); }).writeDefFile(outputDir);

"Done writing FFT synthdefs to:".postln;
outputDir.postln;

0.exit;
