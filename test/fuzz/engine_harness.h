// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Sam Aaron
//
// One SuperSonic engine per fuzzing process: scsynth as Clockwork's guest,
// headless, rendered and controlled by the calling thread (manual pump,
// freewheel clock, the host running the control pass), so an input renders
// the same blocks every time it runs and nothing else touches the engine.
#pragma once

#include "ClockworkEngine.h"
#include "GuestConfigText.h"

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace fuzz {

// The engine, booted on first use. Small limits keep each input cheap and the
// memory an input can claim bounded.
inline ClockworkEngine& engine() {
    static ClockworkEngine* e = [] {
        auto* e = new ClockworkEngine();
        ClockworkEngine::Config cfg;
        cfg.sampleRate        = 48000;
        cfg.bufferSize        = 128;
        cfg.shmSegment        = false;
        cfg.headless          = true;
        cfg.numOutputChannels = 2;
        cfg.numInputChannels  = 2;
        cfg.manualAudioPump   = true;
        cfg.freewheelClock    = true;
        cfg.hostDrivesControl = true;
        clockwork::guest_config_text::set(cfg.guestConfig, "numBuffers", "64");
        clockwork::guest_config_text::set(cfg.guestConfig, "maxNodes", "256");
        clockwork::guest_config_text::set(cfg.guestConfig, "maxGraphDefs", "256");
        clockwork::guest_config_text::set(cfg.guestConfig, "maxWireBufs", "64");
        e->onReply = [](const uint8_t*, uint32_t) {};
        e->onDebug = [](const std::string&) {};
        e->init(cfg);
        return e;
    }();
    return *e;
}

// Render `blocks` blocks, each followed by a control pass: enough for a
// command's later stages (the non-real-time half of /d_recv, a /b_alloc) to
// run and answer.
inline void run(uint32_t blocks) {
    for (uint32_t i = 0; i < blocks; ++i) {
        engine().pumpAudioBlock();
        engine().controlPass();
    }
}

inline void send(const uint8_t* data, size_t size) {
    engine().sendOSC(data, static_cast<uint32_t>(size));
}

// An OSC message: the address, ",<tags>", then the arguments, each 4-aligned.
class Message {
public:
    explicit Message(const char* address) { str(address); mTags = ","; }
    Message& i(int32_t v) { mTags += 'i'; be(static_cast<uint32_t>(v)); return *this; }
    Message& f(float v) {
        uint32_t u;
        std::memcpy(&u, &v, sizeof u);
        mTags += 'f';
        be(u);
        return *this;
    }
    Message& s(const char* v) {
        mTags += 's';
        mArgs.insert(mArgs.end(), v, v + std::strlen(v));
        mArgs.push_back(0);
        while (mArgs.size() % 4) mArgs.push_back(0);
        return *this;
    }
    Message& b(const uint8_t* data, size_t n) {
        mTags += 'b';
        be(static_cast<uint32_t>(n));
        mArgs.insert(mArgs.end(), data, data + n);
        while (mArgs.size() % 4) mArgs.push_back(0);
        return *this;
    }
    std::vector<uint8_t> bytes() const {
        std::vector<uint8_t> out = mAddr;
        std::vector<uint8_t> tags(mTags.begin(), mTags.end());
        tags.push_back(0);
        while (tags.size() % 4) tags.push_back(0);
        out.insert(out.end(), tags.begin(), tags.end());
        out.insert(out.end(), mArgs.begin(), mArgs.end());
        return out;
    }
private:
    void str(const char* s) {
        const size_t n = std::strlen(s);
        mAddr.assign(s, s + n);
        mAddr.push_back(0);
        while (mAddr.size() % 4) mAddr.push_back(0);
    }
    void be(uint32_t v) {
        for (int k = 3; k >= 0; --k) mArgs.push_back(static_cast<uint8_t>(v >> (8 * k)));
    }
    std::vector<uint8_t> mAddr, mArgs;
    std::string mTags;
};

inline void send(const Message& m) {
    const auto b = m.bytes();
    send(b.data(), b.size());
}

// The SynthDefs SuperSonic ships, loaded by name from the package.
inline void loadShippedDef(const char* name) {
    const auto path = std::filesystem::path(SUPERSONIC_SYNTHDEFS_DIR) / (std::string(name) + ".scsyndef");
    std::ifstream f(path, std::ios::binary);
    const std::vector<uint8_t> data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (data.empty()) __builtin_trap();   // the build points at the package; a missing def is a broken harness
    send(Message("/d_recv").b(data.data(), data.size()));
    run(4);
}

} // namespace fuzz
