@@ -0,0 +1,43 @@
# SuperSonic

SuperSonic is a port of SuperCollider's scsynth audio engine to work within the strict constraints of a web audioworklet.

This means that:

1. It is compiled from c++ to WASM
2. It has to comply with the strict requirements of Audioworklets:
   * No thread spawning
   * No malloc
   * No IO
   * No main() entry point
   * No automatic C++ intialiser calls
3. It has to be a static library that gets called via process() from the audioworklet's high priority thread.

The original scsynth was multi-threaded. It had a separate thread for IO vs the audio graph calculations. This was all well coordinated with queue between the threads for passing OSC messages, etc.

We have had to bypass all of this. Instead we have SharedBuffer memory that can be accessed by both the WASM audioworklet code and JS. We use this to ship OSC in and out of our wasm scsynth in addition to shipping debug IO messages for development and info.

We have successfully managed to make sounds using some of the sonic pi synths such as sonic-pi_prophet and sonic-pi_beep. We're currently working on the audio buffer integration.


## Building

You can build SuperSonic with:

./build.sh

This compiles all the assets and places the results in the dist dir.

## Running

Start the test server from the example directory:

cd example && npx serve


## Example

We have a nice demo example in example/demo.html which has a BOOT button to initialise scsynth, a text area for writing ascii OSC (with initial timestamps) and three other boxes - Debug Info (showing debug output from scsynth), OSC In and OSC Out showing OSC in and out of scsynth.

