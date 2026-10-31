// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Sam Aaron
//
// scsynth's command handlers (SC_MiscCmds.cpp, SC_SequencedCommand.cpp) and
// the unit generators they start, over any sequence of OSC packets. An input
// is packets, each a 2-byte big-endian length then that many bytes; each is
// sent to the engine and a block rendered after it, with SynthDefs loaded so
// /s_new starts real graphs whose controls the input sets.
//
// Refused before sending: any packet naming a verb that reads or writes a
// file, quits, or reaches the network (Clockwork's own /clockwork/ verbs are
// fuzzed in Clockwork). Those verbs act outside the process by design, so a
// fuzzer must not run them; the bytes are matched anywhere in the packet, so
// a bundle cannot carry one in either.
//
// After each input every node, timed bundle and buffer is freed and /dumpOSC is
// turned off, so inputs do not build on each other.
#include "engine_harness.h"

#include <algorithm>
#include <string_view>

namespace {

constexpr std::string_view kRefused[] = {
    "/d_load", "/b_allocRead", "/b_read", "/b_write", "/quit", "/clockwork/",
};

bool refused(const uint8_t* p, size_t n) {
    const std::string_view bytes(reinterpret_cast<const char*>(p), n);
    return std::any_of(std::begin(kRefused), std::end(kRefused),
                       [&](std::string_view verb) { return bytes.find(verb) != std::string_view::npos; });
}

void reset() {
    fuzz::send(fuzz::Message("/clearSched"));
    fuzz::send(fuzz::Message("/dumpOSC").i(0));
    fuzz::send(fuzz::Message("/g_freeAll").i(0));
    for (int32_t b = 0; b < 64; ++b) fuzz::send(fuzz::Message("/b_free").i(b));
    fuzz::run(4);
}

// Loaded once, on the first input: libFuzzer and the replay main both call
// only LLVMFuzzerTestOneInput.
void loadDefs() {
    static const bool loaded = [] {
        for (const char* def : { "sonic-pi-beep", "sonic-pi-fm", "sonic-pi-dsaw", "sonic-pi-fx_reverb",
                                 "sonic-pi-basic_mono_player", "sonic-pi-basic_stereo_player" })
            fuzz::loadShippedDef(def);
        return true;
    }();
    (void)loaded;
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    loadDefs();
    size_t at = 0;
    while (at + 2 <= size) {
        const size_t n = (size_t(data[at]) << 8) | data[at + 1];
        at += 2;
        if (n == 0 || at + n > size) break;
        if (!refused(data + at, n)) {
            fuzz::send(data + at, n);
            fuzz::run(1);
        }
        at += n;
    }
    fuzz::run(4);
    reset();
    return 0;
}
