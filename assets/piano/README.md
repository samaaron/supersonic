# Piano wavetable asset

`piano_wavetable.dat` is the sample table for the `MdaPiano` UGen (the `:piano`
synth in Sonic Pi). It used to be compiled into the engine binary as the
`short pianoData[]` array in `src/synth/plugins/sc3-plugins/mdaPianoData.h`
(~1.1 MB of PCM), which was too big for embedded / WASM builds. It now ships as
an external asset that the host loads at boot and injects via
`supersonic_set_piano_wavetable(const short* data, size_t count)`.

## Format

Raw little-endian `int16`, no header: 586349 samples = 1,172,698 bytes. This is
exactly the old `pianoData[]` array dumped in order. The `MdaPiano` UGen indexes
it directly using the keygroup offsets in `mdaPiano_sc3.h`.

## Loading

- **Native**: pass `--piano-wavetable <path>` to the engine. It is read on the
  boot thread (never the audio thread) into an engine-owned buffer and injected.
- **WASM**: the JS host fetches the `.dat`, copies it into the heap with
  `_malloc`, and calls `_supersonic_set_piano_wavetable(ptr, count)`.

If the asset is absent (a deployment that ships without the piano), `MdaPiano`
outputs silence rather than reading a null table.

## Provenance / license

Derived from Paul Kellett's mda Piano VST, ported to SC3 by Dan Stowell. The mda
plug-ins are released under the MIT license or the GPL (v2 or later). See the
header comment in `MdaUGens.cpp`.
