// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Sam Aaron
/*
 * test_supersonic_front.cpp — SuperSonic's socket front: the client gateway
 * that still answers /b_allocRead for a sender who has not heard the engine
 * stopped reading files (front/supersonic_commands.h).
 *
 * Sonic Pi's spider says /b_allocRead bufnum path start frames and waits for
 * /done "/b_allocRead" bufnum, or /fail "/b_allocRead" why bufnum. The front
 * sits between the socket and the engine, decodes the file off the audio
 * thread, stages the frames in the assets area lane with the guest's guard frames,
 * commits them as an asset, and answers in scsynth's words. The engine's own
 * asset replies for those buffers stop at the front; the spider never learns
 * a lane exists.
 *
 * The rig here is the front around an in-process engine (FrontRig.h): replies
 * routed by origin come through onReplyRouted, exactly what the fronted
 * transport sees in SuperSonicMain.cpp, and a recording sink stands in for the
 * socket. The front's work is the test thread's, between blocks, so every
 * wait is a count of blocks.
 */
#include "FrontRig.h"
#include "clockwork_audio_file.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <algorithm>
#include <cmath>
#include "TestPid.h"
#include "BlockBudget.h"
#include "AudioThreadWitness.h"
#include "rt_alloc.h"
#include <chrono>
#include <functional>
#include <fstream>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <string>
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

// The front around an in-process engine (FrontRig.h), and the spider's ways to it.
struct Rig : FrontRig {
    using FrontRig::FrontRig;

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
    // Until the lane is whole again (a release lands after /done).
    bool laneWholeWithin(int ms) {
        return fx.pollUntil([&] { return front.laneFreeBytes() == front.laneBytes(); }, ms);
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
    REQUIRE(rig.waitFor("/done", kSpider, "/b_allocRead", &done));
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
    REQUIRE(rig.waitFor("/done", kSpider, "/b_allocRead", nullptr));
    REQUIRE(rig.front.laneFreeBytes() < rig.front.laneBytes());

    // The spider frees, as it always did; the engine answers it, as it always did.
    const auto free = osc_test::message("/b_free", int32_t{0});
    REQUIRE_FALSE(rig.ingress(free));                // not the front's business
    rig.fx.engine().ingest(free.ptr(), free.size(), kSpider);
    REQUIRE(rig.waitFor("/done", kSpider, "/b_free", nullptr));
    CHECK(rig.laneWholeWithin(2000));
    CHECK(rig.sink.count("/clockwork/asset/released", kSpider) == 0);
}

TEST_CASE("front: a file that cannot be decoded fails to the asker, scsynth-style",
          "[front][load_sample]") {
    Rig rig;
    const uint32_t before = rig.front.laneFreeBytes();
    REQUIRE(rig.ingress(allocRead(1, samplePath("no-such-file-here.wav"))));
    OscReply fail;
    REQUIRE(rig.waitFor("/fail", kSpider, "/b_allocRead", &fail));
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
    REQUIRE(rig.waitFor("/done", kSpider, "/b_allocRead", nullptr));
    const uint32_t oneSlot = rig.front.laneBytes() - rig.front.laneFreeBytes();
    REQUIRE(oneSlot > 0);

    rig.sink.clear();
    REQUIRE(rig.ingress(allocRead(2, path, 100, 200)));
    REQUIRE(rig.waitFor("/done", kSpider, "/b_allocRead", nullptr));
    CHECK(rig.queryFrames(2) == 200);

    // Only the new slot is held: the old one came back when the guest let it go.
    rig.fx.pollUntil([&] { return rig.front.laneBytes() - rig.front.laneFreeBytes() < oneSlot; });
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
    REQUIRE(rig.waitFor("/b_info", kSpider, nullptr, &info));
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
    REQUIRE(rig.waitFor("/b_info", kSpider, nullptr, nullptr));
}

TEST_CASE("front: the audio thread never carries the decode", "[front][load_sample]") {
    // Where the decode runs, counted rather than timed (AudioThreadWitness.h): the front's work, never in a
    // block. This thread renders the blocks here (manualAudioPump), so a decode a block carried would be counted.
    if (!haveSample("bd_haus.flac")) SKIP("sample bd_haus.flac not found");
    ClockworkEngine::Config cfg = EngineFixture::defaultConfig();
    cfg.manualAudioPump = true;
    Rig rig(cfg);
    const uint64_t before = audio_thread_witness::fileWorkCount();
    REQUIRE(rig.ingress(allocRead(4, samplePath("bd_haus.flac"))));
    REQUIRE(rig.waitFor("/done", kSpider, "/b_allocRead", nullptr, 10000));
    rig.fx.pumpBlock(50);
    CHECK(audio_thread_witness::fileWorkCount() == before);
    CHECK(rig.queryFrames(4) == static_cast<int32_t>(framesOf(samplePath("bd_haus.flac"))));
}

TEST_CASE("front: file work is counted on the audio thread, and only there", "[front]") {
    // The instrument the cases above and below read. rt_alloc's flag marks the audio thread while a block renders
    // (process_audio's guard); file work outside it, the front's, is not counted.
    const uint64_t before = audio_thread_witness::fileWorkCount();
    audio_thread_witness::fileWork();
    CHECK(audio_thread_witness::fileWorkCount() == before);
    {
        rt_alloc::Guard inBlock;   // as process_audio sets it around the render
        audio_thread_witness::fileWork();
    }
    CHECK(audio_thread_witness::fileWorkCount() == before + 1);
}

TEST_CASE("front, benchmark: blocks rendered while the front decodes stay within a block's budget",
          "[front][load_sample][realtime][benchmark]") {
    // The wall-clock side of the case above: how long the blocks took while the decode ran. A shared runner's
    // blocks measure the runner as much as the work, so this is a benchmark, run by hand; the claim itself is
    // counted above.
    if (!haveSample("bd_haus.flac")) SKIP("sample bd_haus.flac not found");
    block_budget::requireWithin(block_budget::kBlockMs, [&] {
        ClockworkEngine::Config cfg = EngineFixture::defaultConfig();
        cfg.manualAudioPump = true;   // this thread IS the audio thread
        Rig rig(cfg, Rig::Work::Own);

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
        REQUIRE(rig.waitFor("/done", kSpider, "/b_allocRead", nullptr, 10000));
        for (int i = 0; i < 50; ++i) pump();

        CHECK(rig.queryFrames(4) > 0);
        return maxBlockMs;
    });
}

// ── The synthdef verbs: /d_loadDir and /d_load read files too ───────────────
//
// Sonic Pi's boot sends /d_loadDir for its 122 synthdefs, and the engine read
// them all inside one render callback: 24 ms, the one overrun left after
// samples moved to the lane. The front reads the files on its thread and
// hands each to the engine as /d_recv, the next only on the reply to the
// last, so no block parses more than one; the asker hears one /done, in
// scsynth's words.

namespace {
// Whether `name` can be played: /s_new of an unknown synthdef fails.
bool synthKnown(Rig& rig, const char* name, uint32_t token) {
    rig.sink.clear();
    osc_test::Builder b;
    b.begin("/s_new") << name << int32_t{-1} << int32_t{0} << int32_t{0};
    const auto m = b.end();
    rig.fx.engine().ingest(m.ptr(), m.size(), token);
    OscReply fail;
    const bool failed = rig.waitFor("/fail", token, "/s_new", &fail, 400);
    rig.fx.engine().ingest(osc_test::message("/g_freeAll", int32_t{0}).ptr(),
                           osc_test::message("/g_freeAll", int32_t{0}).size(), token);
    return !failed;
}
} // namespace

namespace {
// The longest block rendered while `body` runs, with `pump` handed to it.
struct BlockTimer {
    double maxMs = 0.0;
    std::function<void()> pumpOf(Rig& rig) {
        return [this, &rig] {
            const auto t0 = std::chrono::steady_clock::now();
            rig.fx.pumpBlock();
            const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            if (ms > maxMs) maxMs = ms;
        };
    }
};

std::filesystem::path biggestSynthDef(const std::filesystem::path& dir) {
    std::filesystem::path best;
    uintmax_t bestSize = 0;
    for (auto& e : std::filesystem::directory_iterator(dir)) {
        if (e.path().extension() != ".scsyndef") continue;
        if (e.file_size() > bestSize) { bestSize = e.file_size(); best = e.path(); }
    }
    return best;
}

// The platform's price for one definition: the longest block while the
// engine parses the biggest synthdef in the directory, handed to it directly.
// /d_recv parses on the audio thread by design here (scsynth's stage 2, on
// the one thread there is), so a runner where a single parse runs past one
// block's budget has not caught the front carrying anything. A front that
// carried the read would cost the whole directory in one block.
double singleDefBlockMs(const std::filesystem::path& def) {
    ClockworkEngine::Config cfg = EngineFixture::defaultConfig();
    cfg.manualAudioPump = true;
    Rig rig(cfg, Rig::Work::Own);
    std::vector<uint8_t> bytes;
    {
        std::ifstream f(def, std::ios::binary);
        bytes.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    }
    REQUIRE(!bytes.empty());
    BlockTimer timer;
    const auto pump = timer.pumpOf(rig);
    for (int i = 0; i < 20; ++i) pump();
    const auto pkt = osc_test::messageWithBlob("/d_recv", bytes.data(), bytes.size());
    rig.fx.engine().ingest(pkt.ptr(), pkt.size(), kSpider);
    REQUIRE(rig.waitFor("/done", kSpider, "/d_recv", nullptr, 5000, pump));
    return timer.maxMs;
}
} // namespace

TEST_CASE("front: the engine itself reads no sample files: a sample sent past the front loads nothing", "[front]") {
    // The other half of "the audio thread never carries the decode": the front's decode is counted (above), and the
    // engine has no sound-file reader to carry one with (scsynth's is a stub here). A sample sent straight to it, past
    // the front, loads nothing.
    if (!haveSample("bd_haus.flac")) SKIP("sample bd_haus.flac not found");
    ClockworkEngine::Config cfg = EngineFixture::defaultConfig();
    cfg.manualAudioPump = true;
    Rig rig(cfg);
    const auto read = allocRead(6, samplePath("bd_haus.flac"));
    rig.fx.engine().ingest(read.ptr(), read.size(), kSpider);
    for (int i = 0; i < 100; ++i) rig.fx.pumpBlock();
    CHECK(rig.queryFrames(6) == 0);
    CHECK(rig.sink.count("/done", kSpider) == 0);
}

TEST_CASE("front: /d_loadDir reads the synthdefs off the audio thread; no block carries the read",
          "[front][synthdef]") {
    // What keeps a block from carrying the directory, counted rather than timed: the files are read as the front's
    // work, never in a block (AudioThreadWitness.h; this thread renders the blocks, manualAudioPump), and the
    // definitions go to the engine a batch a callback, no more than the callback's share of bytes (or one alone).
    if (!std::filesystem::is_directory(CLOCKWORK_SYNTHDEFS_DIR)) SKIP("no synthdef dir");
    ClockworkEngine::Config cfg = EngineFixture::defaultConfig();
    cfg.manualAudioPump = true;
    Rig rig(cfg);
    REQUIRE_FALSE(synthKnown(rig, "sonic-pi-beep", kSpider));

    const uint64_t before = audio_thread_witness::fileWorkCount();
    rig.sink.clear();
    REQUIRE(rig.ingress(osc_test::message("/d_loadDir", CLOCKWORK_SYNTHDEFS_DIR)));
    REQUIRE(rig.waitFor("/done", kSpider, "/d_loadDir", nullptr, 30000));
    rig.fx.pumpBlock(50);
    CHECK(audio_thread_witness::fileWorkCount() == before);
    CHECK(rig.front.defBatchesOverShare() == 0);

    // One /done for the whole directory — the /d_recv replies stayed behind
    // the front — and the synthdefs are really there.
    CHECK(rig.sink.count("/done", kSpider) == 1);
    CHECK(rig.sink.count("/fail", kSpider) == 0);
    CHECK(synthKnown(rig, "sonic-pi-beep", kSpider));
    CHECK(synthKnown(rig, "sonic-pi-piano", kSpider));
}

TEST_CASE("front, benchmark: blocks rendered while /d_loadDir loads stay within the budget",
          "[front][synthdef][realtime][benchmark]") {
    // The wall-clock side of the case above, run by hand: a shared runner's blocks measure the runner (a Windows
    // one, 0.89.0's release: 6.8 ms for blocks that carried one 0.2 ms definition each).
    if (!std::filesystem::is_directory(CLOCKWORK_SYNTHDEFS_DIR)) SKIP("no synthdef dir");
    const double oneDefMs = singleDefBlockMs(biggestSynthDef(CLOCKWORK_SYNTHDEFS_DIR));
    // Within a block's budget, or within twice what this machine pays for
    // its biggest single definition — whichever is more.
    const double bar = std::max(block_budget::kBlockMs, 2.0 * oneDefMs);
    INFO("one definition costs " << oneDefMs << " ms in a block here; bar " << bar << " ms");
    block_budget::requireWithin(bar, [&] {
        ClockworkEngine::Config cfg = EngineFixture::defaultConfig();
        cfg.manualAudioPump = true;
        Rig rig(cfg, Rig::Work::Own);
        REQUIRE_FALSE(synthKnown(rig, "sonic-pi-beep", kSpider));

        BlockTimer timer;
        const auto pump = timer.pumpOf(rig);
        rig.sink.clear();
        REQUIRE(rig.ingress(osc_test::message("/d_loadDir", CLOCKWORK_SYNTHDEFS_DIR)));
        OscReply done;
        REQUIRE(rig.waitFor("/done", kSpider, "/d_loadDir", &done, 30000, pump));
        for (int i = 0; i < 50; ++i) pump();

        // One /done for the whole directory — the /d_recv replies stayed behind
        // the front — and the synthdefs are really there.
        CHECK(rig.sink.count("/done", kSpider) == 1);
        CHECK(rig.sink.count("/fail", kSpider) == 0);
        CHECK(synthKnown(rig, "sonic-pi-beep", kSpider));
        CHECK(synthKnown(rig, "sonic-pi-piano", kSpider));
        return timer.maxMs;
    });
}

namespace {
// The audio a /d_loadDir of the synthdefs takes with a device of `frames` a
// callback, in frames, counted from the ask to the answer rather than timed:
// this thread renders each callback (manualAudioPump), then runs the control
// pass and the front's work, which is when a device's host gets to them.
uint64_t framesToLoadDefs(uint32_t frames) {
    Rig rig;
    REQUIRE(rig.testMovesAll());
    REQUIRE(rig.ingress(osc_test::message("/d_loadDir", CLOCKWORK_SYNTHDEFS_DIR)));
    uint64_t rendered = 0;
    while (rig.sink.count("/done", kSpider) == 0) {
        REQUIRE(rig.sink.count("/fail", kSpider) == 0);
        REQUIRE(rendered < uint64_t{48000} * 60);   // a minute of audio: stuck, not slow
        rig.fx.pumpCallback(frames);
        rendered += frames;
    }
    return rendered;
}
} // namespace

TEST_CASE("front: a /d_loadDir takes no more audio with a bigger device buffer", "[front][synthdef]") {
    // Sonic Pi's boot waits 5 s for its synthdefs. Handed over one a callback,
    // they took as many callbacks as there are definitions, so a bigger buffer
    // made the load longer: Sonic Pi's 164 on a headless engine, 3.5 s at 1024
    // frames and 7 s at 2048, past the boot's wait.
    if (!std::filesystem::is_directory(CLOCKWORK_SYNTHDEFS_DIR)) SKIP("no synthdef dir");
    const uint64_t at128  = framesToLoadDefs(128);
    const uint64_t at1024 = framesToLoadDefs(1024);
    const uint64_t at4096 = framesToLoadDefs(4096);
    INFO("frames to load: " << at128 << " at 128, " << at1024 << " at 1024, " << at4096 << " at 4096");
    // Within two callbacks of the smallest buffer's: a load's first definition
    // goes alone, with no callback yet measured to share out, and its last
    // batch takes a callback however little is left for it.
    CHECK(at1024 <= at128 + 2 * 1024);
    CHECK(at4096 <= at128 + 2 * 4096);
}

TEST_CASE("front: a callback is handed definitions up to its share of bytes, and always at least one",
          "[front][synthdef]") {
    // The definitions go to the engine a batch a callback: as many as fit the callback's share, 64 bytes for each
    // frame it renders (Commands::kDefBytesPerFrame, measured from the callback before), and always at least one, so
    // one bigger than the share goes alone. A bigger buffer takes more a callback rather than one each, and no
    // callback is handed more than its share.
    if (!std::filesystem::is_directory(CLOCKWORK_SYNTHDEFS_DIR)) SKIP("no synthdef dir");
    for (const uint32_t frames : { 128u, 1024u }) {
        INFO(frames << " frames a callback");
        Rig rig;
        REQUIRE(rig.testMovesAll());
        REQUIRE(rig.ingress(osc_test::message("/d_loadDir", CLOCKWORK_SYNTHDEFS_DIR)));
        for (int i = 0; rig.sink.count("/done", kSpider) == 0; ++i) {
            REQUIRE(rig.sink.count("/fail", kSpider) == 0);
            REQUIRE(i < 100000);
            rig.fx.pumpCallback(frames);
        }
        const auto batch = rig.front.largestDefBatch();
        INFO("largest batch: " << batch.defs << " definitions, " << batch.bytes << " bytes, under a share of "
             << batch.budget);
        CHECK(rig.front.defBatchesOverShare() == 0);
        if (frames == 1024) {
            CHECK(batch.defs > 1);
            CHECK(batch.budget == supersonic::Commands::kDefBytesPerFrame * frames);
        }
    }
}

TEST_CASE("front, measure: what each definition costs the block that parses it", "[front][synthdef][benchmark]") {
    // By hand, for a design question: how many definitions a block can take.
    // Each definition in the directory (SUPERSONIC_MEASURE_SYNTHDEFS, or the
    // package's) goes to the engine as /d_recv on its own, and the block that
    // answers it is timed against a quiet block just before; seven rounds, and
    // the median of each.
    const char* asked = std::getenv("SUPERSONIC_MEASURE_SYNTHDEFS");
    const std::filesystem::path dir = asked && *asked ? asked : CLOCKWORK_SYNTHDEFS_DIR;
    if (!std::filesystem::is_directory(dir)) SKIP("no synthdef dir");
    std::vector<std::pair<std::string, std::vector<uint8_t>>> defs;
    for (const auto& e : std::filesystem::recursive_directory_iterator(dir)) {
        if (!e.is_regular_file() || e.path().extension() != ".scsyndef") continue;
        std::ifstream f(e.path(), std::ios::binary);
        defs.push_back({ std::filesystem::relative(e.path(), dir).string(),
                         std::vector<uint8_t>(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()) });
    }
    std::sort(defs.begin(), defs.end());
    REQUIRE(!defs.empty());

    ClockworkEngine::Config cfg = EngineFixture::defaultConfig();
    cfg.manualAudioPump   = true;
    cfg.hostDrivesControl = true;
    EngineFixture fx(cfg);
    auto count = [&](const char* address) {
        size_t n = 0;
        for (const auto& r : fx.routedReplies()) if (r.origin == kSpider && r.address == address) ++n;
        return n;
    };
    auto answered = [&] { return count("/done"); };
    auto refused  = [&] { return count("/fail"); };
    auto timedBlock = [&] {
        const auto t0 = std::chrono::steady_clock::now();
        fx.engine().pumpAudioBlock();
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        fx.engine().controlPass();
        return ms;
    };
    auto median = [](std::vector<double> v) {
        std::sort(v.begin(), v.end());
        return v[v.size() / 2];
    };

    constexpr int kRounds = 7;
    std::vector<std::vector<double>> parsing(defs.size()), quiet(defs.size());
    for (int round = 0; round < kRounds; ++round) {
        for (size_t i = 0; i < defs.size(); ++i) {
            quiet[i].push_back(timedBlock());
            const size_t before = answered(), refusedBefore = refused();
            const auto pkt = osc_test::messageWithBlob("/d_recv", defs[i].second.data(), defs[i].second.size());
            REQUIRE(fx.engine().ingest(pkt.ptr(), pkt.size(), kSpider) == CLOCKWORK_OK);
            double longest = 0.0;
            for (int blocks = 0; answered() == before && refused() == refusedBefore; ++blocks) {
                INFO(defs[i].first);
                REQUIRE(blocks < 16);
                longest = std::max(longest, timedBlock());
            }
            INFO(defs[i].first << " was refused: the engine answered /fail");
            REQUIRE(refused() == refusedBefore);
            parsing[i].push_back(longest);
        }
    }

    struct Cost { std::string name; size_t bytes; double ms; };
    std::vector<Cost> costs;
    std::vector<double> quietAll;
    double total = 0.0;
    for (size_t i = 0; i < defs.size(); ++i) {
        const double ms = std::max(0.0, median(parsing[i]) - median(quiet[i]));
        costs.push_back({ defs[i].first, defs[i].second.size(), ms });
        quietAll.insert(quietAll.end(), quiet[i].begin(), quiet[i].end());
        total += ms;
    }
    std::sort(costs.begin(), costs.end(), [](const Cost& a, const Cost& b) { return a.ms > b.ms; });
    std::vector<double> each;
    for (const auto& c : costs) each.push_back(c.ms);
    std::printf("\n%zu definitions from %s; a quiet 128-frame block: %.3f ms (budget %.3f ms)\n",
                defs.size(), dir.string().c_str(), median(quietAll), block_budget::kBlockMs);
    std::printf("parse cost: median %.3f ms, 90th %.3f ms, most %.3f ms, all %.1f ms\n",
                median(each), each[each.size() / 10], costs.front().ms, total);
    for (size_t i = 0; i < std::min<size_t>(10, costs.size()); ++i)
        std::printf("  %7.3f ms  %7zu bytes  %s\n", costs[i].ms, costs[i].bytes, costs[i].name.c_str());
}

TEST_CASE("front: on threads of its own, as the server runs it, a sample and a directory of definitions load",
          "[front][rt]") {
    // Every other case here runs the front's work on the test thread (FrontRig.h). The server runs it on the
    // front's own worker, beside a device's audio thread: the hand-over between the two is this case's.
    if (!haveSample("bd_haus.flac")) SKIP("sample bd_haus.flac not found");
    if (!std::filesystem::is_directory(CLOCKWORK_SYNTHDEFS_DIR)) SKIP("no synthdef dir");
    Rig rig(EngineFixture::driverConfig());
    REQUIRE(rig.work == Rig::Work::Own);

    REQUIRE(rig.ingress(allocRead(0, samplePath("bd_haus.flac"))));
    REQUIRE(rig.waitFor("/done", kSpider, "/b_allocRead", nullptr, 10000));
    CHECK(rig.queryFrames(0) == static_cast<int32_t>(framesOf(samplePath("bd_haus.flac"))));

    REQUIRE(rig.ingress(osc_test::message("/d_loadDir", CLOCKWORK_SYNTHDEFS_DIR)));
    REQUIRE(rig.waitFor("/done", kSpider, "/d_loadDir", nullptr, 30000));
    CHECK(rig.sink.count("/fail", kSpider) == 0);
    CHECK(synthKnown(rig, "sonic-pi-beep", kSpider));
}

TEST_CASE("front: /d_load loads what its pattern matches, and says so in scsynth's words when nothing does",
          "[front][synthdef]") {
    if (!std::filesystem::is_directory(CLOCKWORK_SYNTHDEFS_DIR)) SKIP("no synthdef dir");
    ClockworkEngine::Config cfg = EngineFixture::defaultConfig();
    cfg.manualAudioPump = true;
    Rig rig(cfg);
    const std::string dir = CLOCKWORK_SYNTHDEFS_DIR;

    // A glob: the b's, not the p's.
    const auto completion = osc_test::message("/b_query", int32_t{5});
    osc_test::Builder b;
    b.begin("/d_load") << (dir + "/sonic-pi-b*.scsyndef").c_str()
                       << osc::Blob(completion.ptr(), completion.size());
    REQUIRE(rig.ingress(b.end()));
    REQUIRE(rig.waitFor("/done", kSpider, "/d_load", nullptr, 10000));
    REQUIRE(rig.waitFor("/b_info", kSpider, nullptr, nullptr, 2000));   // the completion ran
    CHECK(synthKnown(rig, "sonic-pi-beep", kSpider));
    CHECK_FALSE(synthKnown(rig, "sonic-pi-piano", kSpider));

    // A single file, no wildcard.
    rig.sink.clear();
    REQUIRE(rig.ingress(osc_test::message("/d_load", (dir + "/sonic-pi-piano.scsyndef").c_str())));
    REQUIRE(rig.waitFor("/done", kSpider, "/d_load", nullptr, 5000));
    CHECK(synthKnown(rig, "sonic-pi-piano", kSpider));

    // Nothing matches: /fail "/d_load" <pattern>, as the engine used to say.
    rig.sink.clear();
    const std::string none = dir + "/no-such-*.scsyndef";
    REQUIRE(rig.ingress(osc_test::message("/d_load", none.c_str())));
    OscReply fail;
    REQUIRE(rig.waitFor("/fail", kSpider, "/d_load", &fail, 5000));
    CHECK(fail.parsed().argString(1) == none);

    // A directory that is not there: /fail "/d_loadDir" <dir>.
    rig.sink.clear();
    REQUIRE(rig.ingress(osc_test::message("/d_loadDir", "/no/such/dir/anywhere")));
    REQUIRE(rig.waitFor("/fail", kSpider, "/d_loadDir", &fail, 5000));
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
    REQUIRE(rig.waitFor("/done", kSpider, "/b_read", &done));
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
    REQUIRE(rig.waitFor("/fail", kSpider, "/b_read", &fail));
    CHECK(fail.parsed().argString(1).find("Channel mismatch") != std::string::npos);
    CHECK(fail.parsed().argInt(2) == 4);
    CHECK(rig.laneWholeWithin(1000));

    rig.sink.clear();
    {
        osc_test::Builder b;
        b.begin("/b_read") << int32_t{4} << path.c_str() << int32_t{0} << int32_t{-1} << int32_t{0} << int32_t{1};
        REQUIRE(rig.ingress(b.end()));
    }
    REQUIRE(rig.waitFor("/fail", kSpider, "/b_read", &fail));
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
    REQUIRE(rig.waitFor("/done", kSpider, "/b_readChannel", nullptr));
    const auto mono = firstSamplesOf(rig, 5, 8);
    for (int32_t i = 0; i < 8; ++i) CHECK(mono[i] == Catch::Approx(file.samples[i * ch + 1]).margin(1e-6f));

    // Channels [1, 0] into a new buffer: two channels, swapped.
    {
        osc_test::Builder b;
        b.begin("/b_allocReadChannel") << int32_t{6} << path.c_str() << int32_t{0} << int32_t{0} << int32_t{1} << int32_t{0};
        REQUIRE(rig.ingress(b.end()));
    }
    OscReply done;
    REQUIRE(rig.waitFor("/done", kSpider, "/b_allocReadChannel", &done));
    CHECK(done.parsed().argInt(1) == 6);
    CHECK(rig.queryFrames(6) == (int32_t)file.info.frames);
    const auto swapped = firstSamplesOf(rig, 6, 8);
    for (int32_t f = 0; f < 4; ++f) {
        CHECK(swapped[f * 2 + 0] == Catch::Approx(file.samples[f * ch + 1]).margin(1e-6f));
        CHECK(swapped[f * 2 + 1] == Catch::Approx(file.samples[f * ch + 0]).margin(1e-6f));
    }

    // A channel the file does not have is refused.
    rig.sink.clear();
    {
        osc_test::Builder b;
        b.begin("/b_allocReadChannel") << int32_t{7} << path.c_str() << int32_t{0} << int32_t{0} << int32_t{9};
        REQUIRE(rig.ingress(b.end()));
    }
    OscReply fail;
    REQUIRE(rig.waitFor("/fail", kSpider, "/b_allocReadChannel", &fail));
    CHECK(fail.parsed().argInt(2) == 7);
}

TEST_CASE("front: /b_write round-trips the buffer through WAV and FLAC, encoded here from the returns area",
          "[front][load_sample]") {
    if (!haveSample("bd_haus.flac")) SKIP("sample bd_haus.flac not found");
    const std::string path = samplePath("bd_haus.flac");
    const DecodedFile file = decodeFileFor(path);
    Rig rig;
    REQUIRE(rig.ingress(allocRead(0, path)));
    REQUIRE(rig.waitFor("/done", kSpider, "/b_allocRead", nullptr));
    const uint32_t returnsWhole = rig.front.returnsFreeBytes();
    REQUIRE(returnsWhole > 0);

    struct Case { const char* header; const char* sample; const char* ext; float tolerance; };
    const Case cases[] = {
        { "wav",  "int16", "wav",  1.0f / 32768 * 1.01f },
        { "wav",  "float", "wav",  0.0f },
        { "flac", "int16", "flac", 1.0f / 32768 * 1.01f },
    };
    for (const auto& c : cases) {
        DYNAMIC_SECTION(c.header << " " << c.sample) {
            const std::string out = tempFile("front-b_write", c.ext);
            rig.sink.clear();
            osc_test::Builder b;
            b.begin("/b_write") << int32_t{0} << out.c_str() << c.header << c.sample << int32_t{-1} << int32_t{0} << int32_t{0};
            REQUIRE(rig.ingress(b.end()));
            OscReply done;
            REQUIRE(rig.waitFor("/done", kSpider, "/b_write", &done, 10000));
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
            CHECK(rig.front.returnsFreeBytes() == returnsWhole);
        }
    }

    // A range: 100 frames from frame 50.
    {
        const std::string out = tempFile("front-b_write-range", "wav");
        rig.sink.clear();
        osc_test::Builder b;
        b.begin("/b_write") << int32_t{0} << out.c_str() << "wav" << "float" << int32_t{100} << int32_t{50} << int32_t{0};
        REQUIRE(rig.ingress(b.end()));
        REQUIRE(rig.waitFor("/done", kSpider, "/b_write", nullptr, 10000));
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
    REQUIRE(rig.waitFor("/done", kSpider, "/b_allocRead", nullptr));
    const std::string out = tempFile("front-b_write", "mp3");
    rig.sink.clear();
    osc_test::Builder b;
    b.begin("/b_write") << int32_t{0} << out.c_str() << "mp3" << "int16" << int32_t{-1} << int32_t{0} << int32_t{0};
    REQUIRE(rig.ingress(b.end()));
    OscReply fail;
    REQUIRE(rig.waitFor("/fail", kSpider, "/b_write", &fail));
    CHECK(fail.parsed().argString(1).find("Cannot write") != std::string::npos);
    CHECK(fail.parsed().argInt(2) == 0);
    CHECK_FALSE(std::filesystem::exists(out));
    CHECK(rig.sink.count("/b_info", kSpider) == 0);
}

// A cold swap on the test's own thread, as a device change does it. The
// rebuild's setup is on the instance's ring when this returns, and the front
// hears it with the next pass, after anything the old instance said
// (clockwork's docs/RESTARTS.md).
void coldSwap(Rig& rig) {
    const auto swap = rig.fx.engine().switchDevice("", 44100);
    REQUIRE(swap.success);
    REQUIRE(swap.type == SwapType::Cold);
}

TEST_CASE("front: a sample loaded before a cold swap is loaded after it, its slot still held",
          "[front][load_sample][ColdSwap]") {
    // The frames are the front's, in the lane a rebuild keeps, and the engine hands the new World every asset the
    // old one held (clockwork's docs/RESTARTS.md): nothing is loaded again, and the slot is held until the buffer
    // is freed, as before the swap.
    if (!haveSample("bd_haus.flac")) SKIP("sample bd_haus.flac not found");
    const auto frames = static_cast<int32_t>(framesOf(samplePath("bd_haus.flac")));
    Rig rig;
    REQUIRE(rig.ingress(allocRead(0, samplePath("bd_haus.flac"))));
    REQUIRE(rig.waitFor("/done", kSpider, "/b_allocRead", nullptr));
    const uint32_t held = rig.front.laneFreeBytes();
    REQUIRE(held < rig.front.laneBytes());

    coldSwap(rig);
    CHECK(rig.queryFrames(0) == frames);
    CHECK(rig.front.laneFreeBytes() == held);

    // the new World takes a load as the first did (the first's /done forgotten, so the wait is for this one's)
    rig.sink.clear();
    REQUIRE(rig.ingress(allocRead(1, samplePath("bd_haus.flac"))));
    REQUIRE(rig.waitFor("/done", kSpider, "/b_allocRead", nullptr));
    CHECK(rig.queryFrames(1) == frames);

    // and freeing the two gives their slots back, the one from before the swap included
    rig.fx.send(osc_test::message("/b_free", int32_t{0}));
    rig.fx.send(osc_test::message("/b_free", int32_t{1}));
    CHECK(rig.laneWholeWithin(100));
}

TEST_CASE("front: a load the new instance takes before the front hears of the rebuild is done, and its slot held",
          "[front][load_sample][ColdSwap]") {
    // A load asked for as the swap ends goes to the new instance, before the reply pass has handed on its setup.
    // Setup comes first all the same (clockwork's docs/RESTARTS.md), so the load's answer is the new instance's, and
    // the load is that instance's like any other.
    if (!haveSample("bd_haus.flac")) SKIP("sample bd_haus.flac not found");
    Rig rig;
    REQUIRE(rig.testMovesAll());
    coldSwap(rig);
    REQUIRE(rig.ingress(allocRead(0, samplePath("bd_haus.flac"))));
    rig.front.runPending();   // decoded, staged and committed, before the pass that hands on setup
    rig.fx.pumpBlock();       // the new instance takes it; the pass hands on setup, then its answer

    REQUIRE(rig.waitFor("/done", kSpider, "/b_allocRead", nullptr));
    CHECK(rig.sink.count("/fail", kSpider) == 0);
    CHECK(rig.queryFrames(0) == static_cast<int32_t>(framesOf(samplePath("bd_haus.flac"))));
    CHECK(rig.front.laneFreeBytes() < rig.front.laneBytes());
}

TEST_CASE("front: a load the old instance took just before the rebuild is done to its asker, and the new instance has it",
          "[front][load_sample][ColdSwap]") {
    // The old instance takes the commit, and the rebuild comes before the front has heard so. Its answer comes
    // before setup all the same: the asker hears /done, and the buffer is handed to the new instance with the old
    // one's others, its slot held.
    if (!haveSample("bd_haus.flac")) SKIP("sample bd_haus.flac not found");
    Rig rig;
    REQUIRE(rig.testMovesAll());
    REQUIRE(rig.ingress(allocRead(0, samplePath("bd_haus.flac"))));
    rig.front.runPending();
    rig.fx.engine().pumpAudioBlock();   // a block, with no reply pass after it: the answer waits on the ring
    coldSwap(rig);

    REQUIRE(rig.waitFor("/done", kSpider, "/b_allocRead", nullptr));
    CHECK(rig.sink.count("/fail", kSpider) == 0);
    CHECK(rig.queryFrames(0) == static_cast<int32_t>(framesOf(samplePath("bd_haus.flac"))));
    CHECK(rig.front.laneFreeBytes() < rig.front.laneBytes());
}

TEST_CASE("front: a load whose commit is still waiting when the engine is rebuilt fails to its asker, and its slot goes back",
          "[front][load_sample][ColdSwap]") {
    // Staged and committed, and the rebuild comes before any block has taken the commit. The commit hands the old
    // instance an asset, so it was written for that instance as much as scsynth's own messages beside it, and is
    // refused with them (clockwork's docs/RESTARTS.md, rule 2): the load fails to its asker in scsynth's words, its
    // slot goes back, and the asker hears none of clockwork's refusals — of the commit, or of the front's own free.
    if (!haveSample("bd_haus.flac")) SKIP("sample bd_haus.flac not found");
    Rig rig;
    REQUIRE(rig.testMovesAll());
    REQUIRE(rig.ingress(allocRead(0, samplePath("bd_haus.flac"))));
    rig.front.runPending();   // the free and the commit, waiting in the in ring with no block to take them
    coldSwap(rig);

    OscReply fail;
    REQUIRE(rig.waitFor("/fail", kSpider, "/b_allocRead", &fail));
    CHECK(fail.parsed().argString(1).find("rebuil") != std::string::npos);
    CHECK(rig.laneWholeWithin(100));
    rig.fx.pumpBlock(8);
    CHECK(rig.sink.count("/fail", kSpider) == 1);
    CHECK(rig.sink.count("/done", kSpider) == 0);
    CHECK(rig.sink.count("/clockwork/error", kSpider) == 0);
}

TEST_CASE("front: loads whose commits are waiting when the engine is rebuilt each fail to their asker once",
          "[front][load_sample][ColdSwap]") {
    // A refusal names the message it refuses, not the load that sent it. Clockwork refuses what waited in the order
    // it was sent, so each refusal settles the oldest load still waiting for its commit's answer, and every one is
    // the front's to settle: none reaches the asker as clockwork's.
    if (!haveSample("bd_haus.flac")) SKIP("sample bd_haus.flac not found");
    Rig rig;
    REQUIRE(rig.testMovesAll());
    REQUIRE(rig.ingress(allocRead(0, samplePath("bd_haus.flac"))));
    REQUIRE(rig.ingress(allocRead(1, samplePath("bd_haus.flac"))));
    rig.front.runPending();
    coldSwap(rig);

    REQUIRE(rig.waitFor("/fail", kSpider, "/b_allocRead", nullptr));
    rig.fx.pumpBlock(8);
    CHECK(rig.sink.count("/fail", kSpider) == 2);
    CHECK(rig.sink.count("/clockwork/error", kSpider) == 0);
    CHECK(rig.laneWholeWithin(100));
}

TEST_CASE("front: a definition load in flight when the engine is rebuilt fails to its asker, and sends no more",
          "[front][synthdef][ColdSwap]") {
    // The front hands a directory's definitions to the engine a batch at a time, the next on the answers to the
    // last; a load's first batch is one /d_recv. When the World they are going into is torn down mid-way, what is
    // in flight is refused by clockwork rather than answered by scsynth. The definitions already in are gone with
    // the old World and the rest have no World to go to: the whole load fails to its asker, in scsynth's words, so
    // it asks the new World — nothing is sent on, and the asker never hears a /done for a directory the new World
    // has none of.
    if (!std::filesystem::is_directory(CLOCKWORK_SYNTHDEFS_DIR)) SKIP("no synthdef dir");
    Rig rig;
    REQUIRE(rig.testMovesAll());
    REQUIRE(rig.ingress(osc_test::message("/d_loadDir", CLOCKWORK_SYNTHDEFS_DIR)));
    rig.front.runPending();   // the first definition handed over, and no block to take it
    REQUIRE(rig.front.largestDefBatch().defs == 1);
    coldSwap(rig);

    OscReply fail;
    REQUIRE(rig.waitFor("/fail", kSpider, "/d_loadDir", &fail));
    CHECK(fail.parsed().argString(1).find("rebuil") != std::string::npos);
    rig.fx.pumpBlock(50);
    CHECK(rig.front.largestDefBatch().defs == 1);
    CHECK(rig.sink.count("/fail", kSpider) == 1);
    CHECK(rig.sink.count("/done", kSpider) == 0);
    CHECK_FALSE(synthKnown(rig, "sonic-pi-beep", kSpider));
}

TEST_CASE("front: a batch of definitions in flight when the engine is rebuilt fails its load once, and none of its answers is the next load's",
          "[front][synthdef][ColdSwap]") {
    // A batch after the first carries more than one /d_recv, and a rebuild refuses every one still in flight: the
    // load fails to its asker once, and the rest of the batch's answers are its own, swallowed, not taken for the
    // next load's. A load sent after the rebuild then loads the whole directory into the new World.
    if (!std::filesystem::is_directory(CLOCKWORK_SYNTHDEFS_DIR)) SKIP("no synthdef dir");
    Rig rig;
    REQUIRE(rig.testMovesAll());
    REQUIRE(rig.ingress(osc_test::message("/d_loadDir", CLOCKWORK_SYNTHDEFS_DIR)));
    // The first batch, one definition, answered at the pass after the first callback; the second, measured from
    // that callback, goes at that pass and is in flight once it returns.
    rig.fx.pumpCallback(1024);
    rig.fx.pumpCallback(1024);
    REQUIRE(rig.front.largestDefBatch().defs > 1);
    coldSwap(rig);

    REQUIRE(rig.waitFor("/fail", kSpider, "/d_loadDir", nullptr));
    rig.fx.pumpBlock(50);
    CHECK(rig.sink.count("/fail", kSpider) == 1);
    CHECK(rig.sink.count("/done", kSpider) == 0);

    rig.sink.clear();
    REQUIRE(rig.ingress(osc_test::message("/d_loadDir", CLOCKWORK_SYNTHDEFS_DIR)));
    REQUIRE(rig.waitFor("/done", kSpider, "/d_loadDir", nullptr, 30000));
    CHECK(rig.sink.count("/fail", kSpider) == 0);
    CHECK(synthKnown(rig, "sonic-pi-beep", kSpider));
}
