// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Sam Aaron
//
// SynthDefs played, not only read: each input is sent as the blob of a
// /d_recv; when it is accepted, a synth of its first def is started at the
// head of the root group and 32 blocks are rendered, so every unit generator
// in the def runs its constructor and its calc function on the inputs the def
// wires to it. Buffers 0 to 3 hold audio for the units that read buffers.
// Everything is freed after each input.
//
// Seeds: every SynthDef SuperSonic ships and the test SynthDefs.
#include "engine_harness.h"

#include <string>

namespace {

// The name of the first def in a SynthDef file, or "" when the bytes do not
// get that far. Versions 1 and 2: "SCgf", version, def count, then the name;
// version 3 puts each def's size before its name.
std::string firstDefName(const uint8_t* data, size_t size) {
    auto be32 = [&](size_t at) {
        return (uint32_t(data[at]) << 24) | (uint32_t(data[at + 1]) << 16) | (uint32_t(data[at + 2]) << 8) | data[at + 3];
    };
    if (size < 10 || std::memcmp(data, "SCgf", 4) != 0) return {};
    size_t at = 10;
    if (be32(4) >= 3) at += 4;
    if (at >= size) return {};
    const size_t len = data[at];
    if (len == 0 || at + 1 + len > size) return {};
    return std::string(reinterpret_cast<const char*>(data + at + 1), len);
}

void allocBuffers() {
    static bool done = false;
    if (done) return;
    done = true;
    for (int32_t b = 0; b < 4; ++b) {
        fuzz::send(fuzz::Message("/b_alloc").i(b).i(1024).i(1 + b % 2));
        fuzz::run(2);
        fuzz::send(fuzz::Message("/b_gen").i(b).s("sine1").i(7).f(1.f).f(0.5f));
        fuzz::run(2);
    }
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (size > 1 << 20) return 0;   // a def past the ingress ring's widest message is refused before the reader
    allocBuffers();
    fuzz::send(fuzz::Message("/d_recv").b(data, size));
    fuzz::run(4);
    const std::string name = firstDefName(data, size);
    if (!name.empty()) {
        fuzz::send(fuzz::Message("/s_new").s(name.c_str()).i(-1).i(0).i(0));
        fuzz::run(32);
        fuzz::send(fuzz::Message("/g_freeAll").i(0));
        fuzz::run(2);
    }
    fuzz::send(fuzz::Message("/d_freeAll"));
    fuzz::run(2);
    return 0;
}
