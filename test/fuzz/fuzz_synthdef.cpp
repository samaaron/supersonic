// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Sam Aaron
//
// scsynth's SynthDef reader (dsp/scsynth/synth/server/SC_GraphDef.cpp) over
// any bytes: each input is sent as the blob of a /d_recv, as a client or a
// file would send it, the engine renders until the def is read and answered,
// and every def is freed again. Under ASan and UBSan a read past the bytes, a
// pointer that overflows, or a def built from a bad count is a crash.
//
// Seeds: every SynthDef SuperSonic ships and the test SynthDefs.
#include "engine_harness.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (size > 1 << 20) return 0;   // a def past the ingress ring's widest message is refused before the reader
    fuzz::send(fuzz::Message("/d_recv").b(data, size));
    fuzz::run(4);
    fuzz::send(fuzz::Message("/d_freeAll"));
    fuzz::run(2);
    return 0;
}
