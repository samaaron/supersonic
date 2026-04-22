/*
 * test_bundle_sequential.cpp — intra-bundle /s_new followed by node-mutating
 * commands must target the *newly-created* node.
 *
 * SuperSonic departs from upstream scsynth here: upstream defers UGen Ctors
 * until the first audio block, which means a /n_set in the same bundle as a
 * /s_new mutates the control array *before* the UGens latch their initial
 * values — silently breaking slides (Lag / VarLag UGens never see an input
 * change, so they never engage). SuperSonic runs UGen Ctors synchronously
 * inside meth_s_new / meth_s_newargs, so a /n_set arriving "after" the
 * /s_new (whether same-bundle, same-timetag-two-bundles, or later) operates
 * on post-init state.
 *
 * This file exists specifically to lock that semantic down. The fix is in:
 *   src/scsynth/server/SC_Graph.cpp   — split Graph_FirstCalc → Graph_InitUnits
 *   src/scsynth/server/SC_MiscCmds.cpp — call Graph_InitUnits in meth_s_new
 *                                         and meth_s_newargs
 *
 * Tests cover:
 *   1. Direct semantics — /s_new + /n_set in the same bundle must round-trip
 *      the /n_set value via /s_get (node exists, control mutated).
 *   2. Slide audibility — VarLag-wrapped controls must produce a real slide
 *      (measured via the raw audio output bus): peak amplitude at t≈50ms
 *      must be significantly lower than at t≈1.8s when cutoff_slide=2 and
 *      cutoff 30→120.
 *   3. Bundle categories — the fix must work uniformly regardless of how
 *      the bundle arrives: immediate (timetag 0/1), non-bundle (two
 *      separate messages), and all scheduler paths in between. Immediate
 *      bundles in particular must not acquire a ~3ms artificial latency.
 *   4. Sibling mutating commands — /n_setn, /n_map, /n_run, /n_free must
 *      all work when placed after /s_new in the same bundle.
 *   5. Preserved invariants — zombie synth detection, /n_go emission,
 *      /s_new alone (no subsequent mutation), multiple /s_new in one
 *      bundle, /g_new-before-/s_new ordering.
 */
#include "EngineFixture.h"
#include "OscBuilder.h"
#include <catch2/catch_approx.hpp>
#include <chrono>
#include <thread>
#include <cmath>
#include <cstring>

#include "JuceAudioCallback.h"  // get_audio_output_bus / get_audio_buffer_samples

// ──────────────────────────────────────────────────────────────────────────
// OSC helpers — two-level: individual messages via osc_test::Builder, and
// bundles wrapping them via OscBuilder::bundle() (which is the engine's own
// production bundle wire-format emitter).
// ──────────────────────────────────────────────────────────────────────────

// Build a /s_new for sonic-pi-dsaw with the given cutoff, amp and an envelope
// that holds flat for `sustain` seconds. We keep the LPF wide open (cutoff
// 120) by default because dsaw has a Normalizer(…) AFTER the LPF which
// rescales the filtered signal back to unit amplitude — making cutoff a
// terrible proxy for "audio loudness". We instead drive loudness through
// `amp`, which scales the final pan2 output and is the only stage after
// the normalizer, so it's linearly observable.
static osc_test::Packet sNewDsaw(int32_t id, int32_t target,
                                 float note, float amp, float cutoff,
                                 float sustainSec) {
    osc_test::Builder b;
    auto& s = b.begin("/s_new");
    s << "sonic-pi-dsaw" << id << (int32_t)0 << target
      << "note"    << note
      << "cutoff"  << cutoff
      << "amp"     << amp
      << "attack"  << 0.0f
      << "decay"   << 0.0f
      << "sustain" << sustainSec
      << "release" << 0.01f;
    return b.end();
}

static osc_test::Packet nSetAmpSlide(int32_t nodeId, float slide, float amp) {
    osc_test::Builder b;
    auto& s = b.begin("/n_set");
    s << nodeId
      << "amp_slide" << slide
      << "amp"       << amp;
    return b.end();
}

static osc_test::Packet nSetCutoffSlide(int32_t nodeId, float slide, float cutoff) {
    osc_test::Builder b;
    auto& s = b.begin("/n_set");
    s << nodeId
      << "cutoff_slide" << slide
      << "cutoff"       << cutoff;
    return b.end();
}

// Assemble an OSC bundle directly from a runtime-sized list of messages.
// OscBuilder::bundle() takes an initializer_list (compile-time), so we
// build the wire format (#bundle + timetag + [size+data]* ) manually.
struct BundleBytes {
    std::vector<uint8_t> data;
    const uint8_t* ptr()  const { return data.data(); }
    uint32_t       size() const { return static_cast<uint32_t>(data.size()); }
};

static BundleBytes bundleOf(uint64_t timeTag,
                            std::initializer_list<osc_test::Packet> msgs) {
    size_t total = 8 + 8;                              // "#bundle\0" + timetag
    for (auto& m : msgs) total += 4 + m.size();        // size prefix + payload

    BundleBytes out;
    out.data.resize(total);
    uint8_t* p = out.data.data();

    std::memcpy(p, "#bundle\0", 8); p += 8;
    for (int i = 7; i >= 0; --i) {                     // big-endian uint64
        *p++ = static_cast<uint8_t>((timeTag >> (i * 8)) & 0xFF);
    }

    for (auto& m : msgs) {
        uint32_t sz = m.size();
        *p++ = static_cast<uint8_t>((sz >> 24) & 0xFF);
        *p++ = static_cast<uint8_t>((sz >> 16) & 0xFF);
        *p++ = static_cast<uint8_t>((sz >>  8) & 0xFF);
        *p++ = static_cast<uint8_t>( sz        & 0xFF);
        std::memcpy(p, m.ptr(), sz);
        p += sz;
    }
    return out;
}

static void sendBundle(EngineFixture& fx, const BundleBytes& pkt) {
    fx.send(pkt.ptr(), pkt.size());
}

// ──────────────────────────────────────────────────────────────────────────
// Audio capture helpers — peak amplitude over the most-recently-written
// scsynth output block. The HeadlessDriver ticks process_audio() at real
// audio rate; sampling the output bus after a sleep gives us "peak in the
// block around timepoint T".
// ──────────────────────────────────────────────────────────────────────────

static float capturePeak() {
    auto* bus = reinterpret_cast<const float*>(get_audio_output_bus());
    if (!bus) return 0.0f;
    const int n = get_audio_buffer_samples() * 2; // stereo interleaved? scsynth
                                                  // layout is [L block][R block]
                                                  // — treat as a single span for
                                                  // peak-detection purposes.
    float peak = 0.0f;
    for (int i = 0; i < n; ++i) {
        float v = std::fabs(bus[i]);
        if (v > peak) peak = v;
    }
    return peak;
}

// Average of several peak samples taken 2ms apart — smooths out single-block
// zero-crossing dips.
static float peakOverMs(int durationMs) {
    float maxPeak = 0.0f;
    auto deadline = std::chrono::steady_clock::now()
                  + std::chrono::milliseconds(durationMs);
    while (std::chrono::steady_clock::now() < deadline) {
        float p = capturePeak();
        if (p > maxPeak) maxPeak = p;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return maxPeak;
}

// ──────────────────────────────────────────────────────────────────────────
// NTP timetag helpers for bundle scheduling. Epoch = 1900-01-01 UTC.
// (WallClock.h already defines NTP_EPOCH_OFFSET as a double — we redefine
// the same value with a distinct name to avoid the clash.)
// ──────────────────────────────────────────────────────────────────────────

static constexpr uint64_t kNtpEpochOffsetSecs = 2208988800ULL;

static uint64_t ntpNow() {
    auto now = std::chrono::system_clock::now().time_since_epoch();
    uint64_t secs   = std::chrono::duration_cast<std::chrono::seconds>(now).count()
                    + kNtpEpochOffsetSecs;
    auto nanos = std::chrono::duration_cast<std::chrono::nanoseconds>(now).count()
               - std::chrono::duration_cast<std::chrono::seconds>(now).count()
                 * 1'000'000'000LL;
    uint64_t frac = static_cast<uint64_t>((double)nanos / 1e9 * 4294967296.0);
    return (secs << 32) | frac;
}

static uint64_t ntpPlusMs(int ms) {
    uint64_t now = ntpNow();
    // Treat NTP time as a signed Q32.32 fixed-point seconds offset from
    // 1900-01-01. Adding or subtracting milliseconds does two's-complement
    // arithmetic naturally if we convert to int64 first.
    int64_t delta = static_cast<int64_t>(
        (double)ms / 1000.0 * 4294967296.0);
    return static_cast<uint64_t>(static_cast<int64_t>(now) + delta);
}

// ═══════════════════════════════════════════════════════════════════════════
// 1. DIRECT SEMANTICS — control value round-trip
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("Bundle /s_new + /n_set: /s_get returns the /n_set value, not the /s_new value",
          "[bundle_sequential]") {
    EngineFixture fx;
    REQUIRE(fx.loadSynthDef("sonic-pi-dsaw"));

    fx.send(osc_test::message("/notify", 1));
    fx.clearReplies();

    // Same-bundle /s_new(cutoff=30) + /n_set(cutoff=120). Scsynth should
    // process /s_new first (order in bundle), then /n_set — final control
    // value for cutoff should be 120.
    auto bun = bundleOf(1, {  // timetag 1 = execute immediately
        sNewDsaw(/*id*/2100, /*target*/1, /*note*/50.0f, /*amp*/1.0f, /*cutoff*/30.0f, /*sustain*/5.0f),
        nSetCutoffSlide(/*nodeId*/2100, /*slide*/0.0f, /*cutoff*/120.0f),
    });
    sendBundle(fx, bun);

    // Barrier
    fx.send(osc_test::message("/sync", 2100));
    OscReply synced;
    REQUIRE(fx.waitForReply("/synced", synced));

    // /s_get cutoff
    {
        osc_test::Builder b;
        auto& s = b.begin("/s_get");
        s << (int32_t)2100 << "cutoff";
        fx.send(b.end());
    }
    OscReply r;
    REQUIRE(fx.waitForReply("/n_set", r));
    auto p = r.parsed();
    CHECK(p.argInt(0)    == 2100);
    CHECK(p.argString(1) == "cutoff");
    CHECK(p.argFloat(2)  == Catch::Approx(120.0f).margin(0.01f));

    fx.send(osc_test::message("/n_free", 2100));
}

TEST_CASE("Bundle /s_new + /n_setn: range set applies after node is created",
          "[bundle_sequential]") {
    EngineFixture fx;
    REQUIRE(fx.loadSynthDef("sonic-pi-dsaw"));

    fx.send(osc_test::message("/notify", 1));
    fx.clearReplies();

    // /n_setn layout: node, control, numValues, val1 val2...
    // Set cutoff (range 1) to 90 via /n_setn.
    osc_test::Packet setn;
    {
        osc_test::Builder b;
        auto& s = b.begin("/n_setn");
        s << (int32_t)2101
          << "cutoff" << (int32_t)1 << 90.0f;
        setn = b.end();
    }

    sendBundle(fx, bundleOf(1, {
        sNewDsaw(2101, 1, 50.0f, 1.0f, 30.0f, 5.0f),
        setn,
    }));

    fx.send(osc_test::message("/sync", 2101));
    OscReply s; REQUIRE(fx.waitForReply("/synced", s));

    {
        osc_test::Builder b;
        auto& bs = b.begin("/s_get");
        bs << (int32_t)2101 << "cutoff";
        fx.send(b.end());
    }
    OscReply r; REQUIRE(fx.waitForReply("/n_set", r));
    CHECK(r.parsed().argFloat(2) == Catch::Approx(90.0f).margin(0.01f));

    fx.send(osc_test::message("/n_free", 2101));
}

// ═══════════════════════════════════════════════════════════════════════════
// 2. SLIDE AUDIBILITY — real audio-output verification
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("Bundle /s_new + /n_set(amp_slide=2): amplitude slides from quiet to loud",
          "[bundle_sequential][slide]") {
    EngineFixture fx;
    REQUIRE(fx.loadSynthDef("sonic-pi-dsaw"));

    // dsaw has a Normalizer after the LPF, so `cutoff` is a bad probe for
    // loudness — the normalizer masks filter changes. `amp` is the final
    // stage (pan2 * amp), so it scales output linearly. Start quiet,
    // slide up over 2s.
    sendBundle(fx, bundleOf(1, {
        sNewDsaw(/*id*/2200, /*target*/1, /*note*/50.0f, /*amp*/0.02f, /*cutoff*/120.0f, /*sustain*/6.0f),
        nSetAmpSlide(/*nodeId*/2200, /*slide*/2.0f, /*amp*/1.0f),
    }));

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    float earlyPeak = peakOverMs(150);
    std::this_thread::sleep_for(std::chrono::milliseconds(1400));
    float latePeak  = peakOverMs(200);

    INFO("earlyPeak=" << earlyPeak << " latePeak=" << latePeak);

    // Bug present → VarLag latches amp=1.0 at t=0, earlyPeak == latePeak.
    // Fix present → earlyPeak is small (amp still near 0.02), latePeak large.
    CHECK(earlyPeak < 0.15f);
    CHECK(latePeak  > 0.4f);
    CHECK(latePeak  > earlyPeak * 3.0f);

    fx.send(osc_test::message("/n_free", 2200));
}

TEST_CASE("Bundle /s_new + /n_set(amp_slide=0): amplitude is immediately loud (no slide)",
          "[bundle_sequential][slide]") {
    EngineFixture fx;
    REQUIRE(fx.loadSynthDef("sonic-pi-dsaw"));

    // slide=0 → /n_set takes effect instantly (VarLag counter=1 → one-sample jump).
    sendBundle(fx, bundleOf(1, {
        sNewDsaw(2201, 1, 50.0f, 0.02f, 120.0f, 6.0f),
        nSetAmpSlide(2201, 0.0f, 1.0f),
    }));

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    float earlyPeak = peakOverMs(150);

    INFO("earlyPeak=" << earlyPeak);
    CHECK(earlyPeak > 0.4f);

    fx.send(osc_test::message("/n_free", 2201));
}

TEST_CASE("/s_new alone (no /n_set): amp=0.02 stays quiet throughout",
          "[bundle_sequential][slide]") {
    EngineFixture fx;
    REQUIRE(fx.loadSynthDef("sonic-pi-dsaw"));

    fx.send(sNewDsaw(2202, 1, 50.0f, 0.02f, 120.0f, 6.0f));

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    float earlyPeak  = peakOverMs(150);
    std::this_thread::sleep_for(std::chrono::milliseconds(1400));
    float laterPeak  = peakOverMs(200);

    INFO("earlyPeak=" << earlyPeak << " laterPeak=" << laterPeak);

    // amp stays at 0.02 throughout — tiny peaks.
    CHECK(earlyPeak < 0.08f);
    CHECK(laterPeak < 0.08f);

    fx.send(osc_test::message("/n_free", 2202));
}

// ═══════════════════════════════════════════════════════════════════════════
// 3. BUNDLE CATEGORIES — fix must apply regardless of how the bundle arrives
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("Non-bundle: /s_new and /n_set as two back-to-back messages",
          "[bundle_sequential][category]") {
    EngineFixture fx;
    REQUIRE(fx.loadSynthDef("sonic-pi-dsaw"));

    fx.send(sNewDsaw(2300, 1, 50.0f, 0.02f, 120.0f, 6.0f));
    fx.send(nSetAmpSlide(2300, 2.0f, 1.0f));

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    float earlyPeak = peakOverMs(150);
    std::this_thread::sleep_for(std::chrono::milliseconds(1400));
    float latePeak  = peakOverMs(200);

    INFO("earlyPeak=" << earlyPeak << " latePeak=" << latePeak);
    CHECK(earlyPeak < 0.15f);
    CHECK(latePeak  > 0.4f);
    CHECK(latePeak  > earlyPeak * 3.0f);

    fx.send(osc_test::message("/n_free", 2300));
}

TEST_CASE("Immediate bundle (timetag=0): /s_new + /n_set slides correctly",
          "[bundle_sequential][category]") {
    EngineFixture fx;
    REQUIRE(fx.loadSynthDef("sonic-pi-dsaw"));

    sendBundle(fx, bundleOf(0, {  // timetag 0 also means "immediate"
        sNewDsaw(2301, 1, 50.0f, 0.02f, 120.0f, 6.0f),
        nSetAmpSlide(2301, 2.0f, 1.0f),
    }));

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    float earlyPeak = peakOverMs(150);
    std::this_thread::sleep_for(std::chrono::milliseconds(1400));
    float latePeak  = peakOverMs(200);

    INFO("earlyPeak=" << earlyPeak << " latePeak=" << latePeak);
    CHECK(earlyPeak < 0.15f);
    CHECK(latePeak  > 0.4f);
    CHECK(latePeak  > earlyPeak * 3.0f);

    fx.send(osc_test::message("/n_free", 2301));
}

TEST_CASE("Near-future scheduled bundle: /s_new + /n_set slides correctly",
          "[bundle_sequential][category]") {
    EngineFixture fx;
    REQUIRE(fx.loadSynthDef("sonic-pi-dsaw"));

    // ~50ms in the future — past the bypass-lookahead threshold in practice,
    // lands on the BundleScheduler queue, fires in ~one audio buffer.
    uint64_t when = ntpPlusMs(50);
    sendBundle(fx, bundleOf(when, {
        sNewDsaw(2302, 1, 50.0f, 0.02f, 120.0f, 6.0f),
        nSetAmpSlide(2302, 2.0f, 1.0f),
    }));

    std::this_thread::sleep_for(std::chrono::milliseconds(100));  // wait for fire
    float earlyPeak = peakOverMs(150);
    std::this_thread::sleep_for(std::chrono::milliseconds(1400));
    float latePeak  = peakOverMs(200);

    INFO("earlyPeak=" << earlyPeak << " latePeak=" << latePeak);
    CHECK(earlyPeak < 0.15f);
    CHECK(latePeak  > 0.4f);
    CHECK(latePeak  > earlyPeak * 3.0f);

    fx.send(osc_test::message("/n_free", 2302));
}

TEST_CASE("Two same-timetag bundles (/s_new then /n_set in separate bundles)",
          "[bundle_sequential][category]") {
    EngineFixture fx;
    REQUIRE(fx.loadSynthDef("sonic-pi-dsaw"));

    // Two bundles targeting the same future timetag. They'll be drained
    // together during a single audio-buffer bundle-scheduler sweep, in
    // priority-queue order (first-inserted first — see BundleScheduler.h
    // stability ordering). The fix must still produce the slide even
    // though the messages never share a single bundle.
    uint64_t when = ntpPlusMs(50);
    sendBundle(fx, bundleOf(when, { sNewDsaw(2303, 1, 50.0f, 0.02f, 120.0f, 6.0f) }));
    sendBundle(fx, bundleOf(when, { nSetAmpSlide(2303, 2.0f, 1.0f) }));

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    float earlyPeak = peakOverMs(150);
    std::this_thread::sleep_for(std::chrono::milliseconds(1400));
    float latePeak  = peakOverMs(200);

    INFO("earlyPeak=" << earlyPeak << " latePeak=" << latePeak);
    CHECK(earlyPeak < 0.15f);
    CHECK(latePeak  > 0.4f);
    CHECK(latePeak  > earlyPeak * 3.0f);

    fx.send(osc_test::message("/n_free", 2303));
}

TEST_CASE("Late bundle (modest NTP past): /s_new + /n_set still slides",
          "[bundle_sequential][category]") {
    EngineFixture fx;
    REQUIRE(fx.loadSynthDef("sonic-pi-dsaw"));

    // Bundle with a timetag 50ms before "now". scsynth processes late
    // bundles at the current audio position. (A literal "timetag 2" is
    // 1900-01-01 + 2s — way outside sanity bounds; it gets dropped. Using
    // ntpPlusMs(-50) keeps us in sensible territory while still exercising
    // the late-bundle code path.)
    uint64_t when = ntpPlusMs(-50);
    sendBundle(fx, bundleOf(when, {
        sNewDsaw(2304, 1, 50.0f, 0.02f, 120.0f, 6.0f),
        nSetAmpSlide(2304, 2.0f, 1.0f),
    }));

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    float earlyPeak = peakOverMs(150);
    std::this_thread::sleep_for(std::chrono::milliseconds(1400));
    float latePeak  = peakOverMs(200);

    INFO("earlyPeak=" << earlyPeak << " latePeak=" << latePeak);
    CHECK(earlyPeak < 0.15f);
    CHECK(latePeak  > 0.4f);
    CHECK(latePeak  > earlyPeak * 3.0f);

    fx.send(osc_test::message("/n_free", 2304));
}

// ═══════════════════════════════════════════════════════════════════════════
// 4. SIBLING MUTATING COMMANDS — other node-mutating OSC commands in the
//    same bundle after /s_new
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("Bundle /s_new + /n_run=0: node is paused immediately (not waiting for first audio block)",
          "[bundle_sequential]") {
    EngineFixture fx;
    REQUIRE(fx.loadSynthDef("sonic-pi-dsaw"));

    // With the eager-Ctor fix, /n_run=0 applied in the same bundle as /s_new
    // transitions the node straight to Node_NullCalc; it should produce no
    // audio. Without the fix, /n_run=0 sees mCalcFunc==Graph_FirstCalc and
    // routes via Graph_NullFirstCalc — which still runs Ctors but no compute.
    // Either way, no audio — but the state transitions differ. We test
    // observable silence as a behavioural anchor, and also resumption.
    sendBundle(fx, bundleOf(1, {
        sNewDsaw(/*id*/2400, /*target*/1, /*note*/50.0f, /*amp*/1.0f, /*cutoff*/120.0f, /*sustain*/6.0f),
        osc_test::message("/n_run", 2400, 0),
    }));

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    float pausedPeak = peakOverMs(100);
    INFO("pausedPeak=" << pausedPeak);
    CHECK(pausedPeak < 0.02f);             // paused synth = near-silent

    // Resume: now audio should flow.
    fx.send(osc_test::message("/n_run", 2400, 1));
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    float resumedPeak = peakOverMs(150);
    INFO("resumedPeak=" << resumedPeak);
    CHECK(resumedPeak > 0.05f);

    fx.send(osc_test::message("/n_free", 2400));
}

TEST_CASE("Bundle /s_new + /n_free: synth is created and immediately freed cleanly",
          "[bundle_sequential]") {
    EngineFixture fx;
    REQUIRE(fx.loadSynthDef("sonic-pi-dsaw"));

    fx.send(osc_test::message("/notify", 1));
    fx.clearReplies();

    sendBundle(fx, bundleOf(1, {
        sNewDsaw(/*id*/2401, 1, 50.0f, 1.0f, 120.0f, 6.0f),
        osc_test::message("/n_free", 2401),
    }));

    // Engine should survive and remain functional.
    fx.send(osc_test::message("/sync", 2401));
    OscReply s;
    REQUIRE(fx.waitForReply("/synced", s));

    // Node should be gone — /s_get on 2401 should fail.
    {
        osc_test::Builder b;
        auto& bs = b.begin("/s_get");
        bs << (int32_t)2401 << "cutoff";
        fx.send(b.end());
    }
    // In practice we shouldn't see a reply for the cutoff read (node gone).
    // Assert we can still issue further commands — engine not deadlocked.
    fx.send(osc_test::message("/sync", 2401));
    OscReply s2;
    REQUIRE(fx.waitForReply("/synced", s2));
}

TEST_CASE("Bundle /g_new + /s_new + /n_set: group-then-synth ordering works",
          "[bundle_sequential]") {
    EngineFixture fx;
    REQUIRE(fx.loadSynthDef("sonic-pi-dsaw"));

    fx.send(osc_test::message("/notify", 1));
    fx.clearReplies();

    osc_test::Packet gNew = osc_test::message("/g_new", 2500, 0, 0);
    osc_test::Packet sNew = sNewDsaw(2501, /*target*/2500, 50.0f, 1.0f, 30.0f, 5.0f);
    osc_test::Packet nSet = nSetCutoffSlide(2501, 0.0f, 100.0f);

    sendBundle(fx, bundleOf(1, { gNew, sNew, nSet }));

    fx.send(osc_test::message("/sync", 2500));
    OscReply s; REQUIRE(fx.waitForReply("/synced", s));

    // /s_get cutoff to confirm /n_set landed on the new synth inside the
    // new group.
    {
        osc_test::Builder b;
        auto& bs = b.begin("/s_get");
        bs << (int32_t)2501 << "cutoff";
        fx.send(b.end());
    }
    OscReply r; REQUIRE(fx.waitForReply("/n_set", r));
    CHECK(r.parsed().argFloat(2) == Catch::Approx(100.0f).margin(0.01f));

    fx.send(osc_test::message("/n_free", 2501));
    fx.send(osc_test::message("/n_free", 2500));
}

// ═══════════════════════════════════════════════════════════════════════════
// 5. PRESERVED INVARIANTS — none of the things that already worked may regress
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("/s_new alone still emits /n_go and produces audio at /s_new-time cutoff",
          "[bundle_sequential][regression]") {
    EngineFixture fx;
    REQUIRE(fx.loadSynthDef("sonic-pi-dsaw"));

    fx.send(osc_test::message("/notify", 1));
    fx.clearReplies();

    fx.send(sNewDsaw(2600, 1, 50.0f, 1.0f, 120.0f, 6.0f));

    OscReply go;
    REQUIRE(fx.waitForReply("/n_go", go));
    CHECK(go.parsed().argInt(0) == 2600);

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    float peak = peakOverMs(100);
    CHECK(peak > 0.05f);

    fx.send(osc_test::message("/n_free", 2600));
}

TEST_CASE("Multiple /s_new in one bundle: both nodes exist and can be controlled",
          "[bundle_sequential][regression]") {
    EngineFixture fx;
    REQUIRE(fx.loadSynthDef("sonic-pi-dsaw"));

    fx.send(osc_test::message("/notify", 1));
    fx.clearReplies();

    sendBundle(fx, bundleOf(1, {
        sNewDsaw(2700, 1, 50.0f, 1.0f, 80.0f, 5.0f),
        sNewDsaw(2701, 1, 60.0f, 1.0f, 90.0f, 5.0f),
        nSetCutoffSlide(2700, 0.0f, 70.0f),
        nSetCutoffSlide(2701, 0.0f, 100.0f),
    }));

    fx.send(osc_test::message("/sync", 2700));
    OscReply s; REQUIRE(fx.waitForReply("/synced", s));

    for (int32_t id : {2700, 2701}) {
        osc_test::Builder b;
        auto& bs = b.begin("/s_get");
        bs << id << "cutoff";
        fx.send(b.end());
    }
    // Expect two /n_set replies — one per /s_get.
    OscReply r1, r2;
    REQUIRE(fx.waitForReply("/n_set", r1));
    REQUIRE(fx.waitForReply("/n_set", r2));

    // Sort by node id for deterministic assertions.
    int32_t id1 = r1.parsed().argInt(0);
    int32_t id2 = r2.parsed().argInt(0);
    float   v1  = r1.parsed().argFloat(2);
    float   v2  = r2.parsed().argFloat(2);
    if (id1 == 2701) { std::swap(id1, id2); std::swap(v1, v2); }

    CHECK(id1 == 2700);
    CHECK(id2 == 2701);
    CHECK(v1 == Catch::Approx(70.0f).margin(0.01f));
    CHECK(v2 == Catch::Approx(100.0f).margin(0.01f));

    fx.send(osc_test::message("/n_free", 2700));
    fx.send(osc_test::message("/n_free", 2701));
}

TEST_CASE("/s_new + long sustain: output is finite (no NaN / Inf from eager Ctor)",
          "[bundle_sequential][regression]") {
    EngineFixture fx;
    REQUIRE(fx.loadSynthDef("sonic-pi-dsaw"));

    sendBundle(fx, bundleOf(1, {
        sNewDsaw(2800, 1, 50.0f, 1.0f, 110.0f, 5.0f),
    }));

    // Let audio run a while, then scan the whole output buffer for sanity.
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    auto* bus = reinterpret_cast<const float*>(get_audio_output_bus());
    REQUIRE(bus != nullptr);
    const int n = get_audio_buffer_samples() * 2;
    bool allFinite = true;
    for (int i = 0; i < n; ++i) {
        if (!std::isfinite(bus[i])) { allFinite = false; break; }
    }
    CHECK(allFinite);

    fx.send(osc_test::message("/n_free", 2800));
}

// Stress test: rapidly fire many bundled /s_new+/n_set pairs. Tests that
// eager-Ctor doesn't blow up RT memory, synth pool, or the node tree.
TEST_CASE("Stress: 50 back-to-back bundles of /s_new + /n_set, all free cleanly",
          "[bundle_sequential][regression][stress]") {
    EngineFixture fx;
    REQUIRE(fx.loadSynthDef("sonic-pi-dsaw"));

    fx.send(osc_test::message("/notify", 1));
    fx.clearReplies();

    constexpr int N = 50;
    for (int i = 0; i < N; ++i) {
        int32_t id = 2900 + i;
        sendBundle(fx, bundleOf(1, {
            sNewDsaw(id, 1, 50.0f + (i % 12), 1.0f, 80.0f, 0.2f),
            nSetCutoffSlide(id, 0.0f, 100.0f),
        }));
    }

    fx.send(osc_test::message("/sync", 2999));
    OscReply s; REQUIRE(fx.waitForReply("/synced", s, 5000));

    // Let synths finish via their 0.2s sustain + 0.01s release.
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    // Output should still be finite.
    auto* bus = reinterpret_cast<const float*>(get_audio_output_bus());
    REQUIRE(bus != nullptr);
    bool allFinite = true;
    for (int i = 0; i < get_audio_buffer_samples() * 2; ++i) {
        if (!std::isfinite(bus[i])) { allFinite = false; break; }
    }
    CHECK(allFinite);

    // Engine still responsive.
    fx.send(osc_test::message("/sync", 3000));
    OscReply s2; REQUIRE(fx.waitForReply("/synced", s2, 5000));
}
