/*
 * test_buffer_alloc_bounds.cpp — buffer allocation and lookup must be safe
 * against hostile /b_alloc / /b_* arguments.
 *
 * Two bounds bugs reachable from OSC (BufAllocCmd reads bufnum/frames/channels
 * with no validation, then calls World_GetNRTBuf + bufAlloc directly):
 *   - bufAlloc computed numFrames*numChannels in 32-bit, so a crafted pair
 *     overflows to a small/negative count: a tiny allocation while the SndBuf
 *     still advertises the huge frame/channel dims → heap OOB in buffer UGens.
 *   - World_GetBuf/World_GetNRTBuf/World_CopySndBuf used `index > mNumSndBufs`
 *     where the arrays hold exactly mNumSndBufs entries, so index ==
 *     mNumSndBufs returned one element past the end.
 *
 * These exercise the functions directly (no engine boot needed) — the inputs
 * are exactly what the OSC layer forwards unchecked.
 */
#include <catch2/catch_test_macros.hpp>
#include "EngineFixture.h"   // boots the engine → initialises the native heap

#include "SC_World.h"        // World, SndBuf, World_GetBuf/GetNRTBuf (inline)
#include "SC_SndBuf.h"       // SndBufUpdates
#include "SC_Prototypes.h"   // bufAlloc
#include "SC_WorldOptions.h" // World_CopySndBuf
#include "SC_Errors.h"       // kSCErr_*

#include <cstring>
#include <vector>

TEST_CASE("bufAlloc: 32-bit overflow of frames*channels is rejected",
          "[buffer][bounds][security]") {
    // A live heap is essential to this test: without the fix the wrapped count
    // makes zalloc *succeed* with a tiny block (the bug), so the allocator must
    // actually work — otherwise a null alloc would mask the overflow behind a
    // spurious kSCErr_Failed and the test would pass for the wrong reason.
    EngineFixture fx;

    // 65536 * 65537 == 0x1'0001'0000; the low 32 bits are 0x10000 (65536), so a
    // 32-bit multiply wraps to a tiny positive count that passes the < 1 guard
    // and under-allocates 256 KB behind a buffer advertising 65537 x 65536.
    SndBuf buf;
    std::memset(&buf, 0, sizeof(buf));

    SCErr err = bufAlloc(&buf, /*numChannels*/ 65536, /*numFrames*/ 65537, 48000.0);

    CHECK(err == kSCErr_Failed);
    CHECK(buf.data == nullptr);  // nothing under-allocated behind huge dims
}

TEST_CASE("bufAlloc: non-positive dimensions are rejected",
          "[buffer][bounds][security]") {
    SndBuf buf;
    std::memset(&buf, 0, sizeof(buf));
    CHECK(bufAlloc(&buf, 0, 1024, 48000.0) == kSCErr_Failed);
    CHECK(bufAlloc(&buf, 2, 0, 48000.0) == kSCErr_Failed);
    CHECK(bufAlloc(&buf, -1, 1024, 48000.0) == kSCErr_Failed);
}

TEST_CASE("bufAlloc: a valid request allocates the exact sample count",
          "[buffer][bounds]") {
    // bufAlloc draws from the native supersonic heap (sc_malloc), which the
    // engine boot initialises — booting a fixture makes the allocator live so
    // this positive control proves the new bound doesn't over-reject.
    EngineFixture fx;

    SndBuf buf;
    std::memset(&buf, 0, sizeof(buf));

    SCErr err = bufAlloc(&buf, /*numChannels*/ 2, /*numFrames*/ 1024, 48000.0);
    REQUIRE(err == kSCErr_None);
    REQUIRE(buf.data != nullptr);
    CHECK(buf.channels == 2);
    CHECK(buf.frames == 1024);
    CHECK(buf.samples == 2048);

    zfree(buf.data);
}

TEST_CASE("World_GetBuf/GetNRTBuf: index == mNumSndBufs clamps instead of "
          "running one past the end", "[buffer][bounds][security]") {
    constexpr uint32 kNumBufs = 4;
    std::vector<SndBuf> rt(kNumBufs);
    std::vector<SndBuf> nrt(kNumBufs);

    World w;
    std::memset(&w, 0, sizeof(w));
    w.mNumSndBufs = kNumBufs;
    w.mSndBufs = rt.data();
    w.mSndBufsNonRealTimeMirror = nrt.data();

    // In range: identity.
    CHECK(World_GetBuf(&w, 0) == &rt[0]);
    CHECK(World_GetBuf(&w, kNumBufs - 1) == &rt[kNumBufs - 1]);

    // One past the end must clamp to slot 0, NOT return &rt[kNumBufs].
    CHECK(World_GetBuf(&w, kNumBufs) == &rt[0]);
    CHECK(World_GetNRTBuf(&w, kNumBufs) == &nrt[0]);

    // Far out of range stays clamped.
    CHECK(World_GetBuf(&w, 0xFFFFFFFFu) == &rt[0]);
}

TEST_CASE("World_CopySndBuf: index == mNumSndBufs is out of range",
          "[buffer][bounds][security]") {
    constexpr uint32 kNumBufs = 4;
    // Oversize the updates array by one so that, if the bounds check regresses,
    // the stale read lands in our own memory (returning kSCErr_None) rather than
    // segfaulting — the test then discriminates on the return code, not a crash.
    std::vector<SndBufUpdates> updates(kNumBufs + 1);
    std::memset(updates.data(), 0, updates.size() * sizeof(SndBufUpdates));

    World w;
    std::memset(&w, 0, sizeof(w));
    w.mNumSndBufs = kNumBufs;
    w.mSndBufUpdates = updates.data();

    SndBuf out;
    std::memset(&out, 0, sizeof(out));
    bool changed = false;

    // onlyIfChanged=true with no recorded change means the in-range path returns
    // without locking/copying, so this isolates the boundary predicate.
    CHECK(World_CopySndBuf(&w, kNumBufs, &out, true, &changed) == kSCErr_IndexOutOfRange);
}

// /b_alloc, /b_free, /b_zero and /b_gen index mSndBufUpdates with the client's
// buffer number, and World_GetBuf clamps an out-of-range number to buffer 0.
// An out-of-range number gets /fail, and buffer 0 is unchanged.
TEST_CASE("Buffer commands: a buffer number the World does not have is refused",
          "[buffer][bounds][security]") {
    EngineFixture fx;
    fx.send(osc_test::message("/b_alloc", 0, 1024, 1));
    OscReply r;
    REQUIRE(fx.waitForReply("/done", r));

    auto refused = [&](const osc_test::Packet& p, const char* cmd) {
        fx.clearReplies();
        fx.send(p);
        REQUIRE(fx.waitForReply("/fail", r));
        CHECK(r.parsed().argString(0) == cmd);
    };
    for (const int32_t bufnum : {int32_t(0x7FFFFFFF), int32_t(-1)}) {
        refused(osc_test::message("/b_free", bufnum), "/b_free");
        refused(osc_test::message("/b_zero", bufnum), "/b_zero");
        refused(osc_test::message("/b_alloc", bufnum, 16, 1), "/b_alloc");
        osc_test::Builder gen;
        gen.begin("/b_gen") << bufnum << "sine1" << int32_t(7) << 1.0f;
        refused(gen.end(), "/b_gen");
    }

    fx.clearReplies();
    fx.send(osc_test::message("/b_query", 0));
    REQUIRE(fx.waitForReply("/b_info", r));
    CHECK(r.parsed().argInt(1) == 1024);
}

// /b_setn and /b_fill hold each range to the buffer's samples. A buffer never
// allocated has no samples to hold a range to, so they write nothing to it.
TEST_CASE("Buffer commands: /b_setn and /b_fill on a buffer with no samples write nothing",
          "[buffer][bounds][security]") {
    EngineFixture fx;
    for (const int32_t start : {int32_t(-1), int32_t(-1000), int32_t(0), int32_t(0x7FFFFFF0)}) {
        INFO("start " << start);
        osc_test::Builder setn;
        setn.begin("/b_setn") << int32_t(0) << start << int32_t(4) << 0.5f << 0.5f << 0.5f << 0.5f;
        fx.send(setn.end());
        osc_test::Builder fill;
        fill.begin("/b_fill") << int32_t(0) << start << int32_t(4) << 0.5f;
        fx.send(fill.end());
    }

    OscReply r;
    fx.clearReplies();
    fx.send(osc_test::message("/sync", 1));
    REQUIRE(fx.waitForReply("/synced", r));
    fx.send(osc_test::message("/b_query", 0));
    REQUIRE(fx.waitForReply("/b_info", r));
    CHECK(r.parsed().argInt(1) == 0);
}

// A /b_alloc that cannot get the memory leaves the buffer as it was: its
// samples and the counts that describe them, so the commands after it read and
// write the buffer that is still there.
TEST_CASE("Buffer commands: a /b_alloc that fails leaves the buffer as it was",
          "[buffer][bounds][security]") {
    EngineFixture fx;
    OscReply r;
    REQUIRE(fx.sendAndExpectDone(osc_test::message("/b_alloc", 0, 1024, 1)));

    // The most samples bufAlloc accepts, more than the engine's memory holds.
    fx.send(osc_test::message("/b_alloc", 0, 0x7FFFFFFF / 4, 1));
    fx.clearReplies();
    fx.send(osc_test::message("/sync", 1));
    REQUIRE(fx.waitForReply("/synced", r));

    osc_test::Builder gen;
    gen.begin("/b_gen") << int32_t(0) << "sine2" << int32_t(7) << 1.0f << 1.0f;
    REQUIRE(fx.sendAndExpectDone(gen.end()));

    fx.clearReplies();
    fx.send(osc_test::message("/b_query", 0));
    REQUIRE(fx.waitForReply("/b_info", r));
    CHECK(r.parsed().argInt(1) == 1024);
    CHECK(r.parsed().argInt(2) == 1);
    fx.clearReplies();
    fx.send(osc_test::message("/b_get", 0, 256));
    REQUIRE(fx.waitForReply("/b_set", r));
    CHECK(r.parsed().argFloat(2) > 0.99f);
}
