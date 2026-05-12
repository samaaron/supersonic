// sclang shim for the AudioOut2 UGen. The implementation lives in
// supersonic's scsynth (src/scsynth/plugins/DelayUGens.cpp); this class
// only exists so sclang can emit .scsyndef files that name the UGen.
// Load at synthdef-compile time with sclang --include-path.
//
// Inputs match the C++ Ctor: input 0 is the slot index, inputs 1..N are
// audio-rate channel signals. Sink UGen (no outputs); the *ar method
// returns 0.0, mirroring ScopeOut2 in BufIO.sc.

AudioOut2 : UGen {
    *ar { arg slot = 0, inputArray;
        this.multiNewList(['audio', slot] ++ inputArray.asArray);
        ^0.0
    }
}
