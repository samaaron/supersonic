// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Sam Aaron
/*
 * test_supersonic_front.cpp — SuperSonic's socket front: the client gateway
 * that still answers /b_allocRead for a sender who has not heard the engine
 * stopped reading files (front/SuperSonicFront.h).
 *
 * Sonic Pi's spider says /b_allocRead bufnum path start frames and waits for
 * /done "/b_allocRead" bufnum, or /fail "/b_allocRead" why bufnum. The front
 * sits between the socket and the engine, decodes the file on a thread of its
 * own, stages the frames in the inbox lane with the guest's guard frames,
 * commits them as an asset, and answers in scsynth's words. The engine's own
 * asset replies for those buffers stop at the front; the spider never learns
 * a lane exists.
 *
 * The rig here is the front around an in-process engine: replies routed by
 * origin come through onReplyRouted, exactly what the fronted transport sees
 * in Main.cpp, and a recording sink stands in for the socket.
 */
#include "EngineFixture.h"
#include "OscTestUtils.h"
#include "SuperSonicFront.h"
#include "clockwork_audio_file.h"
#include "IOscTransport.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <algorithm>
#include <cmath>
#include "TestPid.h"
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifndef CLOCKWORK_SAMPLES_DIR
#define CLOCKWORK_SAMPLES_DIR ""
#endif
#ifndef CLOCKWORK_SYNTHDEFS_DIR
#define CLOCKWORK_SYNTHDEFS_DIR ""
#endif

namespace {

constexpr uint32_t kSpider = 0x5B1D;   // any non-zero token: the sender on the socket

std::string samplePath(const char* name) { return std::string(CLOCKWORK_SAMPLES_DIR) + "/" + name; }
bool haveSample(const char* name) { return std::filesystem::exists(samplePath(name)); }

uint64_t framesOf(const std::string& path) {
    ClockworkAudioInfo info {};
    info.struct_bytes = sizeof info;
    REQUIRE(clockwork_audio_probe_file(path.c_str(), &info) == CLOCKWORK_OK);
    return info.frames;
}

// /b_allocRead bufnum path [start frames [completion]]
osc_test::Packet allocRead(int32_t bufnum, const std::string& path,
                           int32_t start = 0, int32_t frames = 0,
                           const osc_test::Packet* completion = nullptr) {
    osc_test::Builder b;
    auto& s = b.begin("/b_allocRead");
    s << bufnum << path.c_str() << start << frames;
    if (completion) s << osc::Blob(completion->ptr(), completion->size());
    return b.end();
}

// The socket, as the front sees it: every send is kept, by token.
struct RecordingSink final : IOscTransport {
    struct Sent { uint32_t token; OscReply reply; };
    std::mutex        mu;
    std::vector<Sent> sent;

    bool send(uint32_t token, const uint8_t* d, uint32_t n, bool) override {
        std::lock_guard<std::mutex> lock(mu);
        sent.push_back({ token, OscReply{ osc_test::parseAddress(d, n), std::vector<uint8_t>(d, d + n) } });
        return true;
    }
    void broadcastNotify(const uint8_t*, uint32_t) override {}
    void broadcastLink(const uint8_t*, uint32_t) override {}
    bool hasNotifySubscribers() const override { return false; }
    bool subscribeNotify(uint32_t) override { return false; }
    void subscribeNotifyPort(int) override {}
    void unsubscribeNotify(uint32_t) override {}
    void clearNotify() override {}
    bool subscribeLink(uint32_t) override { return false; }
    void unsubscribeLink(uint32_t) override {}
    void broadcastMidi(const uint8_t*, uint32_t) override {}
    bool subscribeMidi(uint32_t) override { return false; }
    void unsubscribeMidi(uint32_t) override {}
    void broadcastGamepad(const uint8_t*, uint32_t) override {}
    bool subscribeGamepad(uint32_t) override { return false; }
    void unsubscribeGamepad(uint32_t) override {}

    // The first reply at `addr` for `token` whose first string argument is
    // `cmd` (scsynth's /done and /fail name the command), within timeoutMs.
    bool waitFor(const char* addr, uint32_t token, const char* cmd, OscReply* out,
                 int timeoutMs = 5000, std::function<void()> tick = {}) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
        while (std::chrono::steady_clock::now() < deadline) {
            {
                std::lock_guard<std::mutex> lock(mu);
                for (const auto& s : sent) {
                    if (s.token != token || s.reply.address != addr) continue;
                    if (cmd && (s.reply.parsed().argCount() < 1 || s.reply.parsed().argString(0) != cmd)) continue;
                    if (out) *out = s.reply;
                    return true;
                }
            }
            if (tick) tick(); else std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        return false;
    }
    size_t count(const char* addr, uint32_t token) {
        std::lock_guard<std::mutex> lock(mu);
        size_t n = 0;
        for (const auto& s : sent) if (s.token == token && s.reply.address == addr) ++n;
        return n;
    }
};

// The front around an in-process engine, wired the way Main.cpp wires it.
struct Rig {
    EngineFixture   fx;
    RecordingSink   sink;
    SuperSonicFront front;

    explicit Rig(ClockworkEngine::Config cfg = EngineFixture::defaultConfig())
        : fx(cfg), front(fx.engine(), &sink) {
        fx.engine().onReplyRouted = [this](uint32_t origin, uint32_t, const uint8_t* d, uint32_t n) {
            if (!front.egress(origin, d, n)) sink.send(origin, d, n, false);
        };
    }
    ~Rig() { fx.engine().onReplyRouted = nullptr; }

    bool ingress(const osc_test::Packet& p, uint32_t token = kSpider) {
        return front.ingress(p.ptr(), p.size(), token);
    }
    int32_t queryFrames(int32_t bufnum) {
        fx.clearReplies();
        fx.send(osc_test::message("/b_query", bufnum));
        OscReply info;
        REQUIRE(fx.waitForReply("/b_info", info));
        return info.parsed().argInt(1);
    }
    // Poll until the lane is whole again (a release lands after /done).
    bool laneWholeWithin(int ms) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
        while (std::chrono::steady_clock::now() < deadline) {
            if (front.laneFreeBytes() == front.laneBytes()) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        return front.laneFreeBytes() == front.laneBytes();
    }
};

} // namespace

TEST_CASE("front: /b_allocRead is answered from the lane, in scsynth's words, to the asker",
          "[front][load_sample]") {
    if (!haveSample("bd_haus.flac")) SKIP("sample bd_haus.flac not found");
    Rig rig;
    const std::string path = samplePath("bd_haus.flac");

    REQUIRE(rig.ingress(allocRead(0, path)));       // taken by the front: the engine never sees it
    OscReply done;
    REQUIRE(rig.sink.waitFor("/done", kSpider, "/b_allocRead", &done));
    CHECK(done.parsed().argInt(1) == 0);

    // The buffer is loaded, at the file's length.
    CHECK(rig.queryFrames(0) == static_cast<int32_t>(framesOf(path)));

    // The lane paid for it, and the asset protocol stayed behind the front.
    CHECK(rig.front.laneFreeBytes() < rig.front.laneBytes());
    CHECK(rig.sink.count("/clockwork/asset/committed", kSpider) == 0);
    CHECK(rig.sink.count("/fail", kSpider) == 0);
}

TEST_CASE("front: /b_free hands the slot back, and the release stays behind the front",
          "[front][load_sample]") {
    if (!haveSample("bd_haus.flac")) SKIP("sample bd_haus.flac not found");
    Rig rig;
    REQUIRE(rig.ingress(allocRead(0, samplePath("bd_haus.flac"))));
    REQUIRE(rig.sink.waitFor("/done", kSpider, "/b_allocRead", nullptr));
    REQUIRE(rig.front.laneFreeBytes() < rig.front.laneBytes());

    // The spider frees, as it always did; the engine answers it, as it always did.
    const auto free = osc_test::message("/b_free", int32_t{0});
    REQUIRE_FALSE(rig.ingress(free));                // not the front's business
    rig.fx.engine().ingest(free.ptr(), free.size(), kSpider);
    REQUIRE(rig.sink.waitFor("/done", kSpider, "/b_free", nullptr));
    CHECK(rig.laneWholeWithin(2000));
    CHECK(rig.sink.count("/clockwork/asset/released", kSpider) == 0);
}

TEST_CASE("front: a file that cannot be decoded fails to the asker, scsynth-style",
          "[front][load_sample]") {
    Rig rig;
    const uint32_t before = rig.front.laneFreeBytes();
    REQUIRE(rig.ingress(allocRead(1, samplePath("no-such-file-here.wav"))));
    OscReply fail;
    REQUIRE(rig.sink.waitFor("/fail", kSpider, "/b_allocRead", &fail));
    // /fail ,ssi "/b_allocRead" why bufnum — the shape Sonic Pi's detect_fail matches.
    REQUIRE(fail.parsed().argCount() == 3);
    CHECK_FALSE(fail.parsed().argString(1).empty());
    CHECK(fail.parsed().argInt(2) == 1);
    CHECK(rig.front.laneFreeBytes() == before);
    CHECK(rig.queryFrames(1) == 0);
}

TEST_CASE("front: start and frame count are honoured, and a load replaces what the buffer held",
          "[front][load_sample]") {
    if (!haveSample("bd_haus.flac")) SKIP("sample bd_haus.flac not found");
    Rig rig;
    const std::string path = samplePath("bd_haus.flac");
    REQUIRE(rig.ingress(allocRead(2, path)));
    REQUIRE(rig.sink.waitFor("/done", kSpider, "/b_allocRead", nullptr));
    const uint32_t oneSlot = rig.front.laneBytes() - rig.front.laneFreeBytes();
    REQUIRE(oneSlot > 0);

    rig.sink.sent.clear();
    REQUIRE(rig.ingress(allocRead(2, path, 100, 200)));
    REQUIRE(rig.sink.waitFor("/done", kSpider, "/b_allocRead", nullptr));
    CHECK(rig.queryFrames(2) == 200);

    // Only the new slot is held: the old one came back when the guest let it go.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (rig.front.laneBytes() - rig.front.laneFreeBytes() >= oneSlot
           && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    CHECK(rig.front.laneBytes() - rig.front.laneFreeBytes() < oneSlot);
    // The /b_free the front issued to make room is its own affair.
    CHECK(rig.sink.count("/done", kSpider) == 1);
}

TEST_CASE("front: the completion message runs once the sample is in, after /done",
          "[front][load_sample]") {
    if (!haveSample("bd_haus.flac")) SKIP("sample bd_haus.flac not found");
    Rig rig;
    const auto completion = osc_test::message("/b_query", int32_t{3});
    REQUIRE(rig.ingress(allocRead(3, samplePath("bd_haus.flac"), 0, 0, &completion)));
    OscReply info;
    REQUIRE(rig.sink.waitFor("/b_info", kSpider, nullptr, &info));
    CHECK(info.parsed().argInt(0) == 3);
    CHECK(info.parsed().argInt(1) > 0);   // queried AFTER the load: the frames are there

    std::lock_guard<std::mutex> lock(rig.sink.mu);
    size_t doneAt = SIZE_MAX, infoAt = SIZE_MAX;
    for (size_t i = 0; i < rig.sink.sent.size(); ++i) {
        if (rig.sink.sent[i].token != kSpider) continue;
        if (rig.sink.sent[i].reply.address == "/done"   && doneAt == SIZE_MAX) doneAt = i;
        if (rig.sink.sent[i].reply.address == "/b_info" && infoAt == SIZE_MAX) infoAt = i;
    }
    REQUIRE(doneAt != SIZE_MAX);
    CHECK(doneAt < infoAt);
}

TEST_CASE("front: everything else passes through untouched", "[front]") {
    Rig rig;
    CHECK_FALSE(rig.ingress(osc_test::message("/status")));
    CHECK_FALSE(rig.ingress(osc_test::message("/b_alloc", int32_t{5}, int32_t{100}, int32_t{1})));
    {
        osc_test::Builder b;   // a buffer verb with no file in it
        b.begin("/b_gen") << int32_t{5} << "sine1" << int32_t{7} << 1.0f;
        CHECK_FALSE(rig.ingress(b.end()));
    }
    // A malformed /b_allocRead is not the front's to answer either: the engine
    // says what is wrong with it.
    CHECK_FALSE(rig.ingress(osc_test::message("/b_allocRead")));
    // Engine replies for buffers the front never loaded go on to the socket.
    const auto q = osc_test::message("/b_query", int32_t{9});
    rig.fx.engine().ingest(q.ptr(), q.size(), kSpider);
    REQUIRE(rig.sink.waitFor("/b_info", kSpider, nullptr, nullptr));
}

TEST_CASE("front: the audio thread never carries the decode", "[front][load_sample][realtime]") {
    if (!haveSample("bd_haus.flac")) SKIP("sample bd_haus.flac not found");
    ClockworkEngine::Config cfg = EngineFixture::defaultConfig();
    cfg.manualAudioPump = true;   // this thread IS the audio thread
    Rig rig(cfg);

    double maxBlockMs = 0.0;
    auto pump = [&] {
        const auto t0 = std::chrono::steady_clock::now();
        rig.fx.pumpBlock();
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        if (ms > maxBlockMs) maxBlockMs = ms;
    };
    for (int i = 0; i < 20; ++i) pump();
    REQUIRE(rig.ingress(allocRead(4, samplePath("bd_haus.flac"))));
    // The front decodes on its own thread while this one keeps rendering.
    REQUIRE(rig.sink.waitFor("/done", kSpider, "/b_allocRead", nullptr, 10000, pump));
    for (int i = 0; i < 50; ++i) pump();

    const double budgetMs = 128.0 / 48000.0 * 1000.0;
    INFO("longest block: " << maxBlockMs << " ms; budget: " << budgetMs << " ms");
    CHECK(maxBlockMs < budgetMs);
    CHECK(rig.queryFrames(4) > 0);
}

// ── The synthdef verbs: /d_loadDir and /d_load read files too ───────────────
//
// Sonic Pi's boot sends /d_loadDir for its 122 synthdefs, and the engine read
// them all inside one render callback: 24 ms, the one overrun left after
// samples moved to the lane. The front reads the files on its thread and
// hands each to the engine as /d_recv, paced so no block parses more than a
// few; the asker hears one /done, in scsynth's words.

namespace {
// Whether `name` can be played: /s_new of an unknown synthdef fails.
bool synthKnown(Rig& rig, const char* name, uint32_t token) {
    rig.sink.sent.clear();
    osc_test::Builder b;
    b.begin("/s_new") << name << int32_t{-1} << int32_t{0} << int32_t{0};
    const auto m = b.end();
    rig.fx.engine().ingest(m.ptr(), m.size(), token);
    OscReply fail;
    const bool failed = rig.sink.waitFor("/fail", token, "/s_new", &fail, 400, [&] { rig.fx.pumpBlock(); });
    rig.fx.engine().ingest(osc_test::message("/g_freeAll", int32_t{0}).ptr(),
                           osc_test::message("/g_freeAll", int32_t{0}).size(), token);
    return !failed;
}
} // namespace

TEST_CASE("front: /d_loadDir reads the synthdefs on its own thread; no block carries the read",
          "[front][synthdef][realtime]") {
    if (!std::filesystem::is_directory(CLOCKWORK_SYNTHDEFS_DIR)) SKIP("no synthdef dir");
    ClockworkEngine::Config cfg = EngineFixture::defaultConfig();
    cfg.manualAudioPump = true;
    Rig rig(cfg);
    REQUIRE_FALSE(synthKnown(rig, "sonic-pi-beep", kSpider));

    double maxBlockMs = 0.0;
    auto pump = [&] {
        const auto t0 = std::chrono::steady_clock::now();
        rig.fx.pumpBlock();
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        if (ms > maxBlockMs) maxBlockMs = ms;
    };
    rig.sink.sent.clear();
    REQUIRE(rig.ingress(osc_test::message("/d_loadDir", CLOCKWORK_SYNTHDEFS_DIR)));
    OscReply done;
    REQUIRE(rig.sink.waitFor("/done", kSpider, "/d_loadDir", &done, 30000, pump));
    for (int i = 0; i < 50; ++i) pump();

    const double budgetMs = 128.0 / 48000.0 * 1000.0;
    INFO("longest block: " << maxBlockMs << " ms; budget: " << budgetMs << " ms");
    CHECK(maxBlockMs < budgetMs);
    // One /done for the whole directory — the /d_recv replies stayed behind
    // the front — and the synthdefs are really there.
    CHECK(rig.sink.count("/done", kSpider) == 1);
    CHECK(rig.sink.count("/fail", kSpider) == 0);
    CHECK(synthKnown(rig, "sonic-pi-beep", kSpider));
    CHECK(synthKnown(rig, "sonic-pi-piano", kSpider));
}

TEST_CASE("front: /d_load loads what its pattern matches, and says so in scsynth's words when nothing does",
          "[front][synthdef]") {
    if (!std::filesystem::is_directory(CLOCKWORK_SYNTHDEFS_DIR)) SKIP("no synthdef dir");
    ClockworkEngine::Config cfg = EngineFixture::defaultConfig();
    cfg.manualAudioPump = true;
    Rig rig(cfg);
    auto pump = [&] { rig.fx.pumpBlock(); };
    const std::string dir = CLOCKWORK_SYNTHDEFS_DIR;

    // A glob: the b's, not the p's.
    const auto completion = osc_test::message("/b_query", int32_t{5});
    osc_test::Builder b;
    b.begin("/d_load") << (dir + "/sonic-pi-b*.scsyndef").c_str()
                       << osc::Blob(completion.ptr(), completion.size());
    REQUIRE(rig.ingress(b.end()));
    REQUIRE(rig.sink.waitFor("/done", kSpider, "/d_load", nullptr, 10000, pump));
    REQUIRE(rig.sink.waitFor("/b_info", kSpider, nullptr, nullptr, 2000, pump));   // the completion ran
    CHECK(synthKnown(rig, "sonic-pi-beep", kSpider));
    CHECK_FALSE(synthKnown(rig, "sonic-pi-piano", kSpider));

    // A single file, no wildcard.
    rig.sink.sent.clear();
    REQUIRE(rig.ingress(osc_test::message("/d_load", (dir + "/sonic-pi-piano.scsyndef").c_str())));
    REQUIRE(rig.sink.waitFor("/done", kSpider, "/d_load", nullptr, 5000, pump));
    CHECK(synthKnown(rig, "sonic-pi-piano", kSpider));

    // Nothing matches: /fail "/d_load" <pattern>, as the engine used to say.
    rig.sink.sent.clear();
    const std::string none = dir + "/no-such-*.scsyndef";
    REQUIRE(rig.ingress(osc_test::message("/d_load", none.c_str())));
    OscReply fail;
    REQUIRE(rig.sink.waitFor("/fail", kSpider, "/d_load", &fail, 5000, pump));
    CHECK(fail.parsed().argString(1) == none);

    // A directory that is not there: /fail "/d_loadDir" <dir>.
    rig.sink.sent.clear();
    REQUIRE(rig.ingress(osc_test::message("/d_loadDir", "/no/such/dir/anywhere")));
    REQUIRE(rig.sink.waitFor("/fail", kSpider, "/d_loadDir", &fail, 5000, pump));
    CHECK(fail.parsed().argString(1) == "/no/such/dir/anywhere");
}

// ── /b_read, /b_readChannel, /b_allocReadChannel, /b_write ──────────────────
//
// The rest of scsynth's file verbs, answered the same way: the front decodes
// or encodes on its thread, frames move through the lanes, and the asker
// hears scsynth's words. These are the engine's old cases for the same
// verbs, moved to the front, plus the channel-picking ones.

namespace {
struct DecodedFile {
    ClockworkAudioInfo info {};
    std::vector<float> samples;
};
DecodedFile decodeFileFor(const std::string& path) {
    DecodedFile d;
    d.info.struct_bytes = sizeof(d.info);
    float* out = nullptr;
    REQUIRE(clockwork_audio_decode_file(path.c_str(), &d.info, &out) == CLOCKWORK_OK);
    d.samples.assign(out, out + d.info.frames * d.info.channels);
    clockwork_audio_free(out);
    return d;
}
// The first `count` interleaved samples of a buffer, through /b_getn.
std::vector<float> firstSamplesOf(Rig& rig, int32_t bufnum, int32_t count) {
    rig.fx.clearReplies();
    osc_test::Builder b;
    b.begin("/b_getn") << bufnum << int32_t{0} << count;
    rig.fx.send(b.end());
    OscReply r;
    REQUIRE(rig.fx.waitForReply("/b_setn", r));
    auto p = r.parsed();
    REQUIRE(p.argInt(2) == count);
    std::vector<float> out;
    for (int32_t i = 0; i < count; ++i) out.push_back(p.argFloat(3 + i));
    return out;
}
std::string tempFile(const char* stem, const char* ext) {
    return (std::filesystem::temp_directory_path()
            / (std::string(stem) + "-" + std::to_string(testPid()) + "." + ext)).string();
}
} // namespace

TEST_CASE("front: /b_read fills an allocated buffer from the file, offsets honoured; the lane is free again",
          "[front][load_sample]") {
    if (!haveSample("bd_haus.flac")) SKIP("sample bd_haus.flac not found");
    const std::string path = samplePath("bd_haus.flac");
    const DecodedFile file = decodeFileFor(path);
    const int32_t ch = (int32_t)file.info.channels;
    Rig rig;
    rig.fx.send(osc_test::message("/b_alloc", 3, (int32_t)1000, ch));
    REQUIRE(rig.fx.waitForDone("/b_alloc"));

    // Read 500 frames from file frame 100 into buffer frame 200.
    osc_test::Builder b;
    b.begin("/b_read") << int32_t{3} << path.c_str() << int32_t{100} << int32_t{500} << int32_t{200} << int32_t{0};
    REQUIRE(rig.ingress(b.end()));
    OscReply done;
    REQUIRE(rig.sink.waitFor("/done", kSpider, "/b_read", &done));
    CHECK(done.parsed().argInt(1) == 3);
    const auto got = firstSamplesOf(rig, 3, (200 + 8) * ch);
    for (int32_t i = 0; i < 200 * ch; ++i) CHECK(got[i] == 0.0f);
    for (int32_t i = 0; i < 8 * ch; ++i)
        CHECK(got[200 * ch + i] == Catch::Approx(file.samples[100 * ch + i]).margin(1e-6f));
    CHECK(rig.laneWholeWithin(1000));   // a copy, not a binding: nothing stays in the lane
    CHECK(rig.sink.count("/done", kSpider) == 1);
}

TEST_CASE("front: /b_read refuses a channel mismatch and leaveOpen, as scsynth refused them",
          "[front][load_sample]") {
    if (!haveSample("bd_haus.flac")) SKIP("sample bd_haus.flac not found");
    const std::string path = samplePath("bd_haus.flac");
    const DecodedFile file = decodeFileFor(path);
    Rig rig;
    rig.fx.send(osc_test::message("/b_alloc", 4, (int32_t)100, (int32_t)(file.info.channels + 1)));
    REQUIRE(rig.fx.waitForDone("/b_alloc"));
    {
        osc_test::Builder b;
        b.begin("/b_read") << int32_t{4} << path.c_str() << int32_t{0} << int32_t{-1} << int32_t{0} << int32_t{0};
        REQUIRE(rig.ingress(b.end()));
    }
    OscReply fail;
    REQUIRE(rig.sink.waitFor("/fail", kSpider, "/b_read", &fail));
    CHECK(fail.parsed().argString(1).find("Channel mismatch") != std::string::npos);
    CHECK(fail.parsed().argInt(2) == 4);
    CHECK(rig.laneWholeWithin(1000));

    rig.sink.sent.clear();
    {
        osc_test::Builder b;
        b.begin("/b_read") << int32_t{4} << path.c_str() << int32_t{0} << int32_t{-1} << int32_t{0} << int32_t{1};
        REQUIRE(rig.ingress(b.end()));
    }
    REQUIRE(rig.sink.waitFor("/fail", kSpider, "/b_read", &fail));
    CHECK(fail.parsed().argString(1).find("leaveOpen") != std::string::npos);
}

TEST_CASE("front: /b_readChannel and /b_allocReadChannel keep the channels named, in that order",
          "[front][load_sample]") {
    if (!haveSample("bd_haus.flac")) SKIP("sample bd_haus.flac not found");
    const std::string path = samplePath("bd_haus.flac");
    const DecodedFile file = decodeFileFor(path);
    if (file.info.channels < 2) SKIP("needs a stereo sample");
    const int32_t ch = (int32_t)file.info.channels;
    Rig rig;

    // Channel 1 of the file into a mono buffer.
    rig.fx.send(osc_test::message("/b_alloc", 5, (int32_t)100, (int32_t)1));
    REQUIRE(rig.fx.waitForDone("/b_alloc"));
    {
        osc_test::Builder b;
        b.begin("/b_readChannel") << int32_t{5} << path.c_str() << int32_t{0} << int32_t{50} << int32_t{0} << int32_t{0} << int32_t{1};
        REQUIRE(rig.ingress(b.end()));
    }
    REQUIRE(rig.sink.waitFor("/done", kSpider, "/b_readChannel", nullptr));
    const auto mono = firstSamplesOf(rig, 5, 8);
    for (int32_t i = 0; i < 8; ++i) CHECK(mono[i] == Catch::Approx(file.samples[i * ch + 1]).margin(1e-6f));

    // Channels [1, 0] into a new buffer: two channels, swapped.
    {
        osc_test::Builder b;
        b.begin("/b_allocReadChannel") << int32_t{6} << path.c_str() << int32_t{0} << int32_t{0} << int32_t{1} << int32_t{0};
        REQUIRE(rig.ingress(b.end()));
    }
    OscReply done;
    REQUIRE(rig.sink.waitFor("/done", kSpider, "/b_allocReadChannel", &done));
    CHECK(done.parsed().argInt(1) == 6);
    CHECK(rig.queryFrames(6) == (int32_t)file.info.frames);
    const auto swapped = firstSamplesOf(rig, 6, 8);
    for (int32_t f = 0; f < 4; ++f) {
        CHECK(swapped[f * 2 + 0] == Catch::Approx(file.samples[f * ch + 1]).margin(1e-6f));
        CHECK(swapped[f * 2 + 1] == Catch::Approx(file.samples[f * ch + 0]).margin(1e-6f));
    }

    // A channel the file does not have is refused.
    rig.sink.sent.clear();
    {
        osc_test::Builder b;
        b.begin("/b_allocReadChannel") << int32_t{7} << path.c_str() << int32_t{0} << int32_t{0} << int32_t{9};
        REQUIRE(rig.ingress(b.end()));
    }
    OscReply fail;
    REQUIRE(rig.sink.waitFor("/fail", kSpider, "/b_allocReadChannel", &fail));
    CHECK(fail.parsed().argInt(2) == 7);
}

TEST_CASE("front: /b_write round-trips the buffer through WAV and FLAC, encoded here from the outbox",
          "[front][load_sample]") {
    if (!haveSample("bd_haus.flac")) SKIP("sample bd_haus.flac not found");
    const std::string path = samplePath("bd_haus.flac");
    const DecodedFile file = decodeFileFor(path);
    Rig rig;
    REQUIRE(rig.ingress(allocRead(0, path)));
    REQUIRE(rig.sink.waitFor("/done", kSpider, "/b_allocRead", nullptr));
    const uint32_t outboxWhole = rig.front.outboxFreeBytes();
    REQUIRE(outboxWhole > 0);

    struct Case { const char* header; const char* sample; const char* ext; float tolerance; };
    const Case cases[] = {
        { "wav",  "int16", "wav",  1.0f / 32768 * 1.01f },
        { "wav",  "float", "wav",  0.0f },
        { "flac", "int16", "flac", 1.0f / 32768 * 1.01f },
    };
    for (const auto& c : cases) {
        DYNAMIC_SECTION(c.header << " " << c.sample) {
            const std::string out = tempFile("front-b_write", c.ext);
            rig.sink.sent.clear();
            osc_test::Builder b;
            b.begin("/b_write") << int32_t{0} << out.c_str() << c.header << c.sample << int32_t{-1} << int32_t{0} << int32_t{0};
            REQUIRE(rig.ingress(b.end()));
            OscReply done;
            REQUIRE(rig.sink.waitFor("/done", kSpider, "/b_write", &done, 10000));
            CHECK(done.parsed().argInt(1) == 0);
            // The front's own /b_query stayed behind it.
            CHECK(rig.sink.count("/b_info", kSpider) == 0);
            CHECK(rig.sink.count("/supersonic/buffer/published", kSpider) == 0);

            const DecodedFile back = decodeFileFor(out);
            CHECK(back.info.frames == file.info.frames);
            CHECK(back.info.channels == file.info.channels);
            CHECK(back.info.sample_rate == file.info.sample_rate);
            REQUIRE(back.samples.size() == file.samples.size());
            float worst = 0.0f;
            for (size_t i = 0; i < file.samples.size(); ++i)
                worst = std::max(worst, std::abs(back.samples[i] - file.samples[i]));
            CHECK(worst <= c.tolerance);
            std::remove(out.c_str());
            CHECK(rig.front.outboxFreeBytes() == outboxWhole);
        }
    }

    // A range: 100 frames from frame 50.
    {
        const std::string out = tempFile("front-b_write-range", "wav");
        rig.sink.sent.clear();
        osc_test::Builder b;
        b.begin("/b_write") << int32_t{0} << out.c_str() << "wav" << "float" << int32_t{100} << int32_t{50} << int32_t{0};
        REQUIRE(rig.ingress(b.end()));
        REQUIRE(rig.sink.waitFor("/done", kSpider, "/b_write", nullptr, 10000));
        const DecodedFile back = decodeFileFor(out);
        CHECK(back.info.frames == 100);
        const size_t ch = file.info.channels;
        for (size_t i = 0; i < 8 * ch; ++i) CHECK(back.samples[i] == file.samples[50 * ch + i]);
        std::remove(out.c_str());
    }
}

TEST_CASE("front: /b_write refuses a format the codecs cannot write, at once, and touches no file",
          "[front][load_sample]") {
    if (!haveSample("bd_haus.flac")) SKIP("sample bd_haus.flac not found");
    Rig rig;
    REQUIRE(rig.ingress(allocRead(0, samplePath("bd_haus.flac"))));
    REQUIRE(rig.sink.waitFor("/done", kSpider, "/b_allocRead", nullptr));
    const std::string out = tempFile("front-b_write", "mp3");
    rig.sink.sent.clear();
    osc_test::Builder b;
    b.begin("/b_write") << int32_t{0} << out.c_str() << "mp3" << "int16" << int32_t{-1} << int32_t{0} << int32_t{0};
    REQUIRE(rig.ingress(b.end()));
    OscReply fail;
    REQUIRE(rig.sink.waitFor("/fail", kSpider, "/b_write", &fail));
    CHECK(fail.parsed().argString(1).find("Cannot write") != std::string::npos);
    CHECK(fail.parsed().argInt(2) == 0);
    CHECK_FALSE(std::filesystem::exists(out));
    CHECK(rig.sink.count("/b_info", kSpider) == 0);
}
