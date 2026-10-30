// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Sam Aaron
//
// The unit generators of every SynthDef SuperSonic ships, given hostile
// control values: the first input byte picks a def, a synth of it is started,
// and the rest of the input is read as (control index, float value) pairs and
// sent as /n_set, eight pairs a block for 32 blocks. The values are whatever
// the bytes say: NaN, infinities, huge and negative buffer numbers, indices
// past the end. Buffers 0 to 3 hold audio for the units that read buffers.
// Everything is freed after each input.
#include "engine_harness.h"

#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>

namespace {

const std::vector<std::string>& defs() {
    static const std::vector<std::string> names = [] {
        std::vector<std::string> out;
        for (const auto& e : std::filesystem::directory_iterator(SUPERSONIC_SYNTHDEFS_DIR))
            if (e.path().extension() == ".scsyndef") out.push_back(e.path().stem().string());
        std::sort(out.begin(), out.end());
        for (const auto& n : out) fuzz::loadShippedDef(n.c_str());
        for (int32_t b = 0; b < 4; ++b) {
            fuzz::send(fuzz::Message("/b_alloc").i(b).i(1024).i(1 + b % 2));
            fuzz::run(2);
            fuzz::send(fuzz::Message("/b_gen").i(b).s("sine1").i(7).f(1.f).f(0.5f));
            fuzz::run(2);
        }
        return out;
    }();
    return names;
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    const auto& names = defs();
    if (size < 1 || names.empty()) return 0;
    fuzz::send(fuzz::Message("/s_new").s(names[data[0] % names.size()].c_str()).i(1000).i(0).i(0));
    fuzz::run(1);
    size_t at = 1;
    for (int block = 0; block < 32 && at + 8 <= size; ++block) {
        fuzz::Message set("/n_set");
        set.i(1000);
        for (int pair = 0; pair < 8 && at + 8 <= size; ++pair, at += 8) {
            int32_t index;
            float value;
            std::memcpy(&index, data + at, 4);
            std::memcpy(&value, data + at + 4, 4);
            set.i(index).f(value);
        }
        fuzz::send(set);
        fuzz::run(1);
    }
    fuzz::run(4);
    fuzz::send(fuzz::Message("/g_freeAll").i(0));
    fuzz::run(2);
    return 0;
}
