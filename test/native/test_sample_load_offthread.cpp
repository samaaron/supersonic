// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Sam Aaron
/*
 * test_sample_load_offthread.cpp — a sample load must not stall the audio
 * thread.
 *
 * With the guest booted mRealTime = false, /b_allocRead decoded the whole
 * file inside one render callback: a 20–150 ms block against a 2.7 ms budget,
 * heard as a glitch on every sample not yet loaded (Sonic Pi, 2026-09-07).
 *
 * The fix is where the decoding happens, not how the guest threads: the
 * CLIENT decodes into the inbox lane and hands the slot over as an asset
 * (SampleLane.h), and the audio thread only binds a pointer. The audio
 * thread here is the test thread (manualAudioPump), so each block's duration
 * is measured directly; the bar is the file's own decode time, measured on
 * this thread first. No block may take a large fraction of it — the decode
 * is done before the commit is even sent.
 */
#include "EngineFixture.h"
#include "SampleLane.h"
#include "clockwork_audio_file.h"

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

namespace {

// A plain 16-bit stereo WAV of `seconds` at 48 kHz, big enough that decoding
// it is a measurable stretch of time on any machine.
std::string writeBigWav(double seconds) {
    const uint32_t rate = 48000, channels = 2;
    const uint32_t frames = static_cast<uint32_t>(seconds * rate);
    const uint32_t dataBytes = frames * channels * 2;
    const auto path = std::filesystem::temp_directory_path()
                    / ("clockwork-offthread-" + std::to_string(::getpid()) + ".wav");
    FILE* f = std::fopen(path.string().c_str(), "wb");
    REQUIRE(f != nullptr);
    auto u32 = [&](uint32_t v) { std::fwrite(&v, 4, 1, f); };
    auto u16 = [&](uint16_t v) { std::fwrite(&v, 2, 1, f); };
    std::fwrite("RIFF", 1, 4, f); u32(36 + dataBytes); std::fwrite("WAVE", 1, 4, f);
    std::fwrite("fmt ", 1, 4, f); u32(16); u16(1); u16(static_cast<uint16_t>(channels));
    u32(rate); u32(rate * channels * 2); u16(static_cast<uint16_t>(channels * 2)); u16(16);
    std::fwrite("data", 1, 4, f); u32(dataBytes);
    std::vector<int16_t> block(4096 * channels);
    for (size_t i = 0; i < block.size(); ++i) block[i] = static_cast<int16_t>((i * 37) & 0x7fff);
    uint32_t left = frames;
    while (left > 0) {
        const uint32_t n = left < 4096 ? left : 4096;
        std::fwrite(block.data(), 2, static_cast<size_t>(n) * channels, f);
        left -= n;
    }
    std::fclose(f);
    return path.string();
}

double decodeMillis(const std::string& path) {
    ClockworkAudioInfo info {};
    info.struct_bytes = sizeof(info);
    float* decoded = nullptr;
    const auto t0 = std::chrono::steady_clock::now();
    const ClockworkStatus st = clockwork_audio_decode_file(path.c_str(), &info, &decoded);
    const auto t1 = std::chrono::steady_clock::now();
    REQUIRE(st == CLOCKWORK_OK);
    REQUIRE(info.frames > 0);
    clockwork_audio_free(decoded);
    return std::chrono::duration<double, std::milli>(t1 - t0).count();
}

} // namespace

TEST_CASE("a sample enters through the lane: no audio block carries the decode",
          "[load_sample][realtime]") {
    // 15 s of stereo at 48 kHz: 2.9 MB on disk, 5.8 MB as float frames in the
    // lane — long enough that a decode inside a block would be unmissable,
    // small enough for the lane as it is sized today.
    const std::string path = writeBigWav(15.0);
    const double decodeMs = decodeMillis(path);
    INFO("decode on a plain thread: " << decodeMs << " ms");
    REQUIRE(decodeMs > 1.0);

    ClockworkEngine::Config cfg = EngineFixture::defaultConfig();
    cfg.manualAudioPump = true;   // this thread IS the audio thread
    EngineFixture fx(cfg);
    REQUIRE(fx.engine().guestInboxBytes() >= 6u * 1024u * 1024u);

    // Render blocks one at a time, timing each. The client's work — decode,
    // free, stage — happens between blocks on this thread and is not timed;
    // the commit is one message, and the blocks that carry it ARE timed:
    // binding a pointer is all the audio thread does.
    double maxBlockMs = 0.0;
    auto pumpTimed = [&](int n) {
        for (int i = 0; i < n; ++i) {
            const auto t0 = std::chrono::steady_clock::now();
            fx.pumpBlock();
            const double ms = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - t0).count();
            if (ms > maxBlockMs) maxBlockMs = ms;
        }
    };
    pumpTimed(20);
    const sample_lane::Staged st = sample_lane::stage(fx, 0, path);
    REQUIRE(st.ok);
    fx.send(st.commit);
    pumpTimed(200);
    OscReply committed;
    REQUIRE(fx.waitForReply("/clockwork/asset/committed", committed));
    std::filesystem::remove(path);

    // The bar: well under the decode, and in any case within one block's
    // budget — a plain WAV decodes in a couple of milliseconds on a fast
    // machine, and a block that stays inside its 128-frame budget has carried
    // no decode either way.
    const double budgetMs = 128.0 / 48000.0 * 1000.0;
    const double bar = decodeMs * 0.5 > budgetMs ? decodeMs * 0.5 : budgetMs;
    INFO("longest block: " << maxBlockMs << " ms; decode: " << decodeMs << " ms; bar: " << bar << " ms");
    CHECK(maxBlockMs < bar);

    // And the sample is really there, at the file's length.
    fx.clearReplies();
    fx.send(osc_test::message("/b_query", int32_t{0}));
    OscReply info;
    REQUIRE(fx.waitForReply("/b_info", info));
    CHECK(info.parsed().argInt(1) == 15 * 48000);

    // Freed, the asset comes back to the client.
    fx.clearReplies();
    fx.send(osc_test::message("/b_free", int32_t{0}));
    OscReply rel;
    REQUIRE(fx.waitForReply("/clockwork/asset/released", rel));
    CHECK(rel.parsed().argInt(0) == 0);
}
