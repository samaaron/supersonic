# SuperSonic

SuperSonic is a port of SuperCollider's scsynth audio engine to work within the strict constraints of a web audioworklet. The goal is low latency and high reliability for long running sessions.

**See [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) for full architectural documentation including message flow diagrams and component relationships.**

## AudioWorklet Constraints

The WASM scsynth runs inside an AudioWorklet which has strict requirements:

* No thread spawning
* No malloc (memory must be pre-allocated)
* No IO
* No main() entry point
* No automatic C++ initialiser calls

It's a static library that gets called via process() from the audioworklet's high priority thread.

The original scsynth was multi-threaded - separate threads for IO vs audio graph calculations, coordinated via queues for passing OSC messages. We've had to bypass all of this. Instead we use SharedArrayBuffer memory that can be accessed by both the WASM audioworklet code and JS to ship OSC in and out.

## Components

There isn't just the scsynth C++ compiled to wasm - it's actually a number of components:

* The WASM audioworklet code (scsynth + scheduler + ringbuffer read/write code)
* The JS prescheduler worker (for storing timestamped OSC bundles scheduled beyond the lookahead threshold)
* The SuperSonic JS code

## Communication Modes

There are two distinct modes with which these components communicate:

1. SAB - via SharedArrayBuffer
2. postMessage - via postMessage calls and the periodic transfer of sections of memory (i.e. containing metrics and mirror-node-tree)

Both modes are first class citizens and are fully supported and tested. SAB mode needs extra headers from the server and requires the browser to have stricter security (so no CDN) however postMessage mode is possible to deploy via CDN.

## Building

You can build SuperSonic with:

./build.sh

This compiles all the assets and places the results in the dist dir.

## Running

Start the test server from the example directory:

cd example && npx serve

## Example

We have a nice demo example in example/demo.html which has a BOOT button to initialise scsynth, a text area for writing ascii OSC (with initial timestamps) and three other boxes - Debug Info (showing debug output from scsynth), OSC In and OSC Out showing OSC in and out of scsynth.
