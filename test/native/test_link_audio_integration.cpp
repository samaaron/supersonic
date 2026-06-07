// test_link_audio_integration.cpp
//
// Integration tests for SuperSonic's Link Audio input subscription
// surface. Spawns a real out-of-process Link peer via
// supersonic_test_link_peer and asserts on what the engine receives.
//
// Designed to fail today against the truncating renderer in
// vendor/LinkAudioInputRenderer.hpp (F3): the peer publishes
// 1024-frame buffers but the renderer caps storage at 512 and reports
// a numFrames mismatch to Link, breaking beat-time continuity →
// receive() returns 0 forever.

#ifdef SUPERSONIC_LINK

#include "EngineFixture.h"
#include "FakeLinkPeerProcess.h"
#include "JuceAudioCallback.h"
#include "OscTestUtils.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <memory>
#include <thread>

namespace {

// Connection-state enum mirrors SuperClock::LinkAudioConnectionState.
constexpr int kStateNotSubscribed = 0;
constexpr int kStateConnecting    = 1;
constexpr int kStateConnected     = 2;
constexpr int kStateDropout       = 3;

// Poll /clock/audio/channels/get until we see a channel published by
// `peerName` (or timeout).
bool waitForChannelVisible(EngineFixture& fx,
                           const std::string& peerName,
                           const std::string& channelName,
                           std::chrono::milliseconds timeout) {
    using clock = std::chrono::steady_clock;
    const auto deadline = clock::now() + timeout;
    while (clock::now() < deadline) {
        fx.clearReplies();
        fx.send(osc_test::message("/clock/audio/channels/get"));
        OscReply reply;
        if (fx.waitForReply("/clock/audio/channels.reply", reply, 200)) {
            const auto p = reply.parsed();
            const int count = p.argInt(0);
            // Per entry: [channelId:s channelName:s peerId:s peerName:s]
            for (int i = 0; i < count; ++i) {
                const int base = 1 + i * 4;
                if (p.argString(base + 1) == channelName &&
                    p.argString(base + 3) == peerName) {
                    return true;
                }
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    return false;
}

// Snapshot the status of one input subscription (peerName, channelName).
// Returns -1 in `state` if not found. Updates `bufferedMs` and
// `sourceNumChannels` from the reply.
struct InputSnapshot {
    int   state = -1;
    int   sourceNumChannels = 0;
    float bufferedMs = 0.0f;
};

InputSnapshot snapshotInput(EngineFixture& fx,
                            const std::string& peerName,
                            const std::string& channelName) {
    InputSnapshot snap;
    fx.clearReplies();
    fx.send(osc_test::message("/clock/audio/inputs/get"));
    OscReply reply;
    if (!fx.waitForReply("/clock/audio/inputs.reply", reply, 500)) return snap;
    const auto p = reply.parsed();
    const int count = p.argInt(0);
    // Per entry: [peerName:s channelName:s busIdx:i sampleRate:i
    //             sourceNumChannels:i bufferedMs:f state:i
    //             droppedSourceBuffers:i networkGapBuffers:i
    //             totalSourceBufferCalls:i duplicateCountCalls:i
    //             latencySeconds:f] (12 args)
    for (int i = 0; i < count; ++i) {
        const int base = 1 + i * 12;
        if (p.argString(base + 0) == peerName &&
            p.argString(base + 1) == channelName) {
            snap.sourceNumChannels = p.argInt(base + 4);
            snap.bufferedMs        = p.argFloat(base + 5);
            snap.state             = p.argInt(base + 6);
            break;
        }
    }
    return snap;
}

}  // namespace

TEST_CASE("LinkAudio: receives audio from peer with 1024-frame buffers",
          "[Link][LinkAudio][integration]") {
    FakeLinkPeerProcess::Options peerOpts;
    peerOpts.name         = "FakeLive";
    peerOpts.loopbackOnly = false;  // TODO: loopback-only once lo0 multicast works
    peerOpts.blockSize    = 1024;   // matches Live's typical engine size
    peerOpts.sampleRate   = 48000;
    peerOpts.channels     = {{"Main", 2, "sine440-880"}};
    FakeLinkPeerProcess peer{peerOpts};
    REQUIRE(peer.ready());

    EngineFixture fx;
    // NetworkWide + publish=1 so LinkAudio is on and the engine can
    // see other peers' channels via link.channels().
    fx.send(osc_test::message("/clock/visibility",         int32_t{2}));
    fx.send(osc_test::message("/clock/audio/publish/set",  int32_t{1}));

    REQUIRE(waitForChannelVisible(fx, "FakeLive", "Main",
                                   std::chrono::seconds(30)));

    {
        osc_test::Builder b;
        auto& s = b.begin("/clock/audio/input/add");
        s << "FakeLive" << "Main" << static_cast<int32_t>(64);
        fx.clearReplies();
        fx.send(b.end());
    }
    OscReply addReply;
    REQUIRE(fx.waitForReply("/clock/audio/input/add.reply", addReply, 1000));
    REQUIRE(addReply.parsed().argInt(0) == 1);

    // Evolution diagnostics — print state every second for 10s.
    InputSnapshot snap{};
    for (int i = 0; i < 10; ++i) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        snap = snapshotInput(fx, "FakeLive", "Main");
        std::fprintf(stderr,
            "[t=%ds] state=%d sourceChans=%d bufferedMs=%.2f\n",
            i + 1, snap.state, snap.sourceNumChannels, snap.bufferedMs);
        if (snap.state == kStateConnected) break;
    }

    CHECK(snap.state == kStateConnected);
    CHECK(snap.sourceNumChannels == 2);
    CHECK(snap.bufferedMs > 5.0f);
}

namespace {

// Count the entries reported by /clock/audio/inputs/get. -1 on parse error.
int countInputs(EngineFixture& fx) {
    fx.clearReplies();
    fx.send(osc_test::message("/clock/audio/inputs/get"));
    OscReply reply;
    if (!fx.waitForReply("/clock/audio/inputs.reply", reply, 500)) return -1;
    return reply.parsed().argInt(0);
}

}  // namespace

TEST_CASE("LinkAudio: /clock/reset clears active input subscriptions",
          "[Link][LinkAudio][integration]") {
    FakeLinkPeerProcess::Options peerOpts;
    peerOpts.name       = "FakeLive";
    peerOpts.channels   = {{"Main", 2, "sine440-880"}};
    FakeLinkPeerProcess peer{peerOpts};
    REQUIRE(peer.ready());

    EngineFixture fx;
    fx.send(osc_test::message("/clock/visibility",        int32_t{2}));
    fx.send(osc_test::message("/clock/audio/publish/set", int32_t{1}));

    REQUIRE(waitForChannelVisible(fx, "FakeLive", "Main",
                                   std::chrono::seconds(30)));

    {
        osc_test::Builder b;
        auto& s = b.begin("/clock/audio/input/add");
        s << "FakeLive" << "Main" << static_cast<int32_t>(64);
        fx.clearReplies();
        fx.send(b.end());
    }
    OscReply addReply;
    REQUIRE(fx.waitForReply("/clock/audio/input/add.reply", addReply, 1000));
    REQUIRE(addReply.parsed().argInt(0) == 1);
    REQUIRE(countInputs(fx) == 1);

    fx.send(osc_test::message("/clock/reset"));
    // Poll until the async reset has cleared the subscription, rather than
    // guessing a fixed delay a loaded runner may overrun.
    CHECK(fx.pollUntil([&] { return countInputs(fx) == 0; }));
}

TEST_CASE("LinkAudio: addLinkAudioInput rejects busIdx in output/input range",
          "[Link][LinkAudio][integration]") {
    FakeLinkPeerProcess::Options peerOpts;
    peerOpts.name     = "FakeLive";
    peerOpts.channels = {{"Main", 2, "sine440-880"}};
    FakeLinkPeerProcess peer{peerOpts};
    REQUIRE(peer.ready());

    // EngineFixture defaults to 2 outputs + 2 inputs. Buses 0-1 are
    // hardware outputs, 2-3 hardware inputs, 4+ private. Writing Link
    // input audio into 0-3 would clobber the engine's I/O.
    EngineFixture fx;
    fx.send(osc_test::message("/clock/visibility",        int32_t{2}));
    fx.send(osc_test::message("/clock/audio/publish/set", int32_t{1}));

    REQUIRE(waitForChannelVisible(fx, "FakeLive", "Main",
                                   std::chrono::seconds(30)));

    auto sendAdd = [&](int32_t busIdx) {
        osc_test::Builder b;
        auto& s = b.begin("/clock/audio/input/add");
        s << "FakeLive" << "Main" << busIdx;
        fx.clearReplies();
        fx.send(b.end());
        OscReply r;
        REQUIRE(fx.waitForReply("/clock/audio/input/add.reply", r, 1000));
        return r.parsed().argInt(0);
    };

    // Reject when the bus pair would land in outputs / inputs.
    CHECK(sendAdd(0) == 0);  // overlaps output bus 0/1
    CHECK(sendAdd(1) == 0);  // overlaps output bus 1 + input 2
    CHECK(sendAdd(2) == 0);  // overlaps input 2/3
    CHECK(sendAdd(3) == 0);  // overlaps input 3 + private 4 (input overlap)

    // First private bus pair: accepted.
    CHECK(sendAdd(4) == 1);
}

TEST_CASE("LinkAudio: addLinkAudioInput rejects bus pair collisions",
          "[Link][LinkAudio][integration]") {
    FakeLinkPeerProcess::Options peerOpts;
    peerOpts.name     = "FakeLive";
    peerOpts.channels = {{"Main", 2, "sine440-880"},
                         {"Aux",  1, "dc:0.25"}};
    FakeLinkPeerProcess peer{peerOpts};
    REQUIRE(peer.ready());

    EngineFixture fx;
    fx.send(osc_test::message("/clock/visibility",        int32_t{2}));
    fx.send(osc_test::message("/clock/audio/publish/set", int32_t{1}));

    REQUIRE(waitForChannelVisible(fx, "FakeLive", "Main",
                                   std::chrono::seconds(30)));
    REQUIRE(waitForChannelVisible(fx, "FakeLive", "Aux",
                                   std::chrono::seconds(5)));

    auto sendAdd = [&](const char* channel, int32_t busIdx) {
        osc_test::Builder b;
        auto& s = b.begin("/clock/audio/input/add");
        s << "FakeLive" << channel << busIdx;
        fx.clearReplies();
        fx.send(b.end());
        OscReply r;
        REQUIRE(fx.waitForReply("/clock/audio/input/add.reply", r, 1000));
        return r.parsed().argInt(0);
    };

    // First subscription claims buses 64 and 65 (always pair).
    CHECK(sendAdd("Main", 64) == 1);

    // Different (peer, channel) at busIdx=65 would overlap bus 65 (R of
    // Main). Reject — silent stomp is worse than failing the request.
    CHECK(sendAdd("Aux", 65) == 0);

    // busIdx=64 same as Main — collision on the L bus. Reject.
    CHECK(sendAdd("Aux", 64) == 0);

    // busIdx=66 is past Main's pair → accepted.
    CHECK(sendAdd("Aux", 66) == 1);
}

// Replacement path: re-adding an existing (peer, channel) must STILL
// check overlap against the OTHER active subscriptions. The
// short-circuit-on-match bug overwrote the busIdx without scanning
// the rest of the list — last-loop-wins silent stomp.
TEST_CASE("LinkAudio: re-adding a subscription rejects overlap with others",
          "[Link][LinkAudio][integration]") {
    FakeLinkPeerProcess::Options peerOpts;
    peerOpts.name     = "FakeLive";
    peerOpts.channels = {{"Main", 2, "sine440-880"},
                         {"Aux",  1, "dc:0.25"}};
    FakeLinkPeerProcess peer{peerOpts};
    REQUIRE(peer.ready());

    EngineFixture fx;
    fx.send(osc_test::message("/clock/visibility",        int32_t{2}));
    fx.send(osc_test::message("/clock/audio/publish/set", int32_t{1}));

    REQUIRE(waitForChannelVisible(fx, "FakeLive", "Main",
                                   std::chrono::seconds(30)));
    REQUIRE(waitForChannelVisible(fx, "FakeLive", "Aux",
                                   std::chrono::seconds(5)));

    auto sendAdd = [&](const char* channel, int32_t busIdx) {
        osc_test::Builder b;
        auto& s = b.begin("/clock/audio/input/add");
        s << "FakeLive" << channel << busIdx;
        fx.clearReplies();
        fx.send(b.end());
        OscReply r;
        REQUIRE(fx.waitForReply("/clock/audio/input/add.reply", r, 1000));
        return r.parsed().argInt(0);
    };

    // Initial layout: Main on bus 64/65, Aux on bus 66/67.
    REQUIRE(sendAdd("Main", 64) == 1);
    REQUIRE(sendAdd("Aux",  66) == 1);

    // Re-add Main onto Aux's pair must be rejected even though
    // (FakeLive, Main) is itself a known sub.
    CHECK(sendAdd("Main", 66) == 0);  // overlaps Aux's L
    CHECK(sendAdd("Main", 67) == 0);  // overlaps Aux's R

    // Main stays at its original bus and Aux is intact.
    {
        const auto mainSnap = snapshotInput(fx, "FakeLive", "Main");
        CHECK(mainSnap.state != kStateNotSubscribed);
    }
    {
        const auto auxSnap  = snapshotInput(fx, "FakeLive", "Aux");
        CHECK(auxSnap.state  != kStateNotSubscribed);
    }
}

TEST_CASE("LinkAudio: setLinkVisibility(Off) clears active input subscriptions",
          "[Link][LinkAudio][integration]") {
    FakeLinkPeerProcess::Options peerOpts;
    peerOpts.name       = "FakeLive";
    peerOpts.channels   = {{"Main", 2, "sine440-880"}};
    FakeLinkPeerProcess peer{peerOpts};
    REQUIRE(peer.ready());

    EngineFixture fx;
    fx.send(osc_test::message("/clock/visibility",        int32_t{2}));
    fx.send(osc_test::message("/clock/audio/publish/set", int32_t{1}));

    REQUIRE(waitForChannelVisible(fx, "FakeLive", "Main",
                                   std::chrono::seconds(30)));

    {
        osc_test::Builder b;
        auto& s = b.begin("/clock/audio/input/add");
        s << "FakeLive" << "Main" << static_cast<int32_t>(64);
        fx.clearReplies();
        fx.send(b.end());
    }
    OscReply addReply;
    REQUIRE(fx.waitForReply("/clock/audio/input/add.reply", addReply, 1000));
    REQUIRE(addReply.parsed().argInt(0) == 1);
    REQUIRE(countInputs(fx) == 1);

    fx.send(osc_test::message("/clock/visibility", int32_t{0}));  // Off
    CHECK(fx.pollUntil([&] { return countInputs(fx) == 0; }));
}

namespace {

// Read `blockSize` samples from a single bus into `out`. Reads
// directly from the audio-thread's bus pool — there's no snapshot
// so a torn-mid-write read is possible, but for amplitude / L≠R
// assertions the race doesn't change the outcome.
bool snapshotBus(uint32_t busIdx, uint32_t blockSize, std::vector<float>& out) {
    const auto* pool = reinterpret_cast<const float*>(get_audio_bus_pool());
    if (!pool) return false;
    const int busCount = get_audio_bus_count();
    if (busIdx >= static_cast<uint32_t>(busCount)) return false;
    out.assign(pool + busIdx * blockSize, pool + (busIdx + 1) * blockSize);
    return true;
}

float peakAbs(const std::vector<float>& samples) {
    float peak = 0.0f;
    for (const auto s : samples) peak = std::max(peak, std::fabs(s));
    return peak;
}

float maxAbsDiff(const std::vector<float>& a, const std::vector<float>& b) {
    if (a.size() != b.size()) return 0.0f;
    float d = 0.0f;
    for (size_t i = 0; i < a.size(); ++i) d = std::max(d, std::fabs(a[i] - b[i]));
    return d;
}

// Wait until `peerName`/`channelName` reaches Connected (state=2) or
// timeout. Returns the final snapshot.
InputSnapshot waitForConnected(EngineFixture& fx,
                                const std::string& peerName,
                                const std::string& channelName,
                                std::chrono::milliseconds timeout) {
    InputSnapshot snap{};
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        snap = snapshotInput(fx, peerName, channelName);
        if (snap.state == kStateConnected) return snap;
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    return snap;
}

}  // namespace

// A. Verify scsynth's audio graph actually consumes Link-delivered
// audio (via In.ar) — bus contents alone would only prove
// drainLinkAudioInputsToBuses wrote bytes somewhere.
// Path: FakeLive → bus 64/65 → stereo_passthrough (In.ar(64,2) →
// Out.ar(0,_)) → engine output bus 0/1.
TEST_CASE("LinkAudio: scsynth In.ar consumes audio from a Link subscription",
          "[Link][LinkAudio][integration]") {
    FakeLinkPeerProcess::Options peerOpts;
    peerOpts.name     = "FakeLive";
    peerOpts.channels = {{"Main", 2, "sine440-880"}};
    FakeLinkPeerProcess peer{peerOpts};
    REQUIRE(peer.ready());

    // Manual pump: this test reads the output bus, so the test thread must be the
    // sole audio-thread writer (no real-time driver) or the read races the drain.
    auto cfg = EngineFixture::defaultConfig();
    cfg.manualAudioPump = true;
    EngineFixture fx(cfg);
    fx.send(osc_test::message("/clock/visibility",        int32_t{2}));
    fx.send(osc_test::message("/clock/audio/publish/set", int32_t{1}));

    REQUIRE(waitForChannelVisible(fx, "FakeLive", "Main",
                                   std::chrono::seconds(30)));

    {
        osc_test::Builder b;
        auto& s = b.begin("/clock/audio/input/add");
        s << "FakeLive" << "Main" << static_cast<int32_t>(64);
        fx.clearReplies();
        fx.send(b.end());
    }
    OscReply addReply;
    REQUIRE(fx.waitForReply("/clock/audio/input/add.reply", addReply, 1000));
    REQUIRE(addReply.parsed().argInt(0) == 1);
    REQUIRE(waitForConnected(fx, "FakeLive", "Main",
                              std::chrono::seconds(30)).state
            == kStateConnected);

    // stereo_passthrough reads In.ar(in_bus, 2) and writes both
    // channels unchanged to (out, out+1). That preserves the peer's
    // L/R distinction end-to-end so we can assert real stereo here.
    REQUIRE(fx.loadSynthDef("stereo_passthrough"));
    {
        osc_test::Builder b;
        auto& s = b.begin("/s_new");
        s << "stereo_passthrough" << static_cast<int32_t>(1000)
          << static_cast<int32_t>(0) << static_cast<int32_t>(1)
          << "in_bus" << 64.0f << "out" << 0.0f;
        fx.send(b.end());
    }
    {
        OscReply r;
        fx.send(osc_test::message("/sync", 42));
        REQUIRE(fx.waitForReply("/synced", r));
    }

    // Poll the output bus — slow CI runners can take longer than a
    // fixed sleep to: (1) let the HeadlessDriver drain Link audio
    // into bus 64, (2) let scsynth run the synth at least once with
    // non-zero In.ar input, (3) settle into a steady stream.
    // pollUntil() pumps a block on this thread before each check (manual mode),
    // so the read below sees freshly-rendered output and can't race the drain.
    const auto* outBus = reinterpret_cast<const float*>(get_audio_output_bus());
    REQUIRE(outBus != nullptr);
    constexpr uint32_t kBlockSize = 128;
    float pL = 0.0f, pR = 0.0f, diff = 0.0f;
    fx.pollUntil([&] {
        std::vector<float> outL(outBus,             outBus +     kBlockSize);
        std::vector<float> outR(outBus + kBlockSize, outBus + 2 * kBlockSize);
        pL   = peakAbs(outL);
        pR   = peakAbs(outR);
        diff = maxAbsDiff(outL, outR);
        return pL > 0.01f && pR > 0.01f && diff > 0.01f;
    }, 5000);
    INFO("output peakL=" << pL << " peakR=" << pR
         << " maxAbsDiff=" << diff);
    CHECK(pL > 0.01f);    // L audio from the Link sub (sine440)
    CHECK(pR > 0.01f);    // R audio from the Link sub (sine880)
    CHECK(diff > 0.01f);  // real stereo — L≠R end-to-end through scsynth
}

// ── B. Multi-subscription: two channels to two bus pairs, no cross-mix ────
TEST_CASE("LinkAudio: concurrent subscriptions write to distinct bus pairs",
          "[Link][LinkAudio][integration]") {
    FakeLinkPeerProcess::Options peerOpts;
    peerOpts.name     = "FakeLive";
    peerOpts.channels = {{"ChanA", 1, "sine440"},   // mono → both buses sine
                         {"ChanB", 1, "dc:0.5"}};   // mono → both buses 0.5
    FakeLinkPeerProcess peer{peerOpts};
    REQUIRE(peer.ready());

    // Manual pump: bus snapshots below must not race a real-time driver.
    auto cfg = EngineFixture::defaultConfig();
    cfg.manualAudioPump = true;
    EngineFixture fx(cfg);
    fx.send(osc_test::message("/clock/visibility",        int32_t{2}));
    fx.send(osc_test::message("/clock/audio/publish/set", int32_t{1}));

    REQUIRE(waitForChannelVisible(fx, "FakeLive", "ChanA",
                                   std::chrono::seconds(30)));
    REQUIRE(waitForChannelVisible(fx, "FakeLive", "ChanB",
                                   std::chrono::seconds(5)));

    auto sendAdd = [&](const char* channel, int32_t busIdx) {
        osc_test::Builder b;
        auto& s = b.begin("/clock/audio/input/add");
        s << "FakeLive" << channel << busIdx;
        fx.clearReplies();
        fx.send(b.end());
        OscReply r;
        REQUIRE(fx.waitForReply("/clock/audio/input/add.reply", r, 1000));
        return r.parsed().argInt(0);
    };
    REQUIRE(sendAdd("ChanA", 64) == 1);  // ChanA → bus 64/65
    REQUIRE(sendAdd("ChanB", 66) == 1);  // ChanB → bus 66/67

    REQUIRE(waitForConnected(fx, "FakeLive", "ChanA",
                              std::chrono::seconds(30)).state == kStateConnected);
    REQUIRE(waitForConnected(fx, "FakeLive", "ChanB",
                              std::chrono::seconds(30)).state == kStateConnected);

    // Poll bus contents — slow CI runners need time for drain to
    // populate both bus pairs after Connected. ChanA carries sine440
    // (peak near 1.0), ChanB carries dc:0.5 (peak ~0.5).
    constexpr uint32_t kBlockSize = 128;
    std::vector<float> bus64, bus66;
    float p64 = 0.0f, p66 = 0.0f, diff = 0.0f;
    fx.pollUntil([&] {
        REQUIRE(snapshotBus(64, kBlockSize, bus64));
        REQUIRE(snapshotBus(66, kBlockSize, bus66));
        p64  = peakAbs(bus64);
        p66  = peakAbs(bus66);
        diff = maxAbsDiff(bus64, bus66);
        return p64 > 0.1f && p66 > 0.3f && diff > 0.1f;
    }, 5000);
    INFO("peak64(sine)=" << p64 << " peak66(dc)=" << p66
         << " maxAbsDiff=" << diff);
    CHECK(p64 > 0.1f);              // sine present
    CHECK(p66 > 0.3f);              // dc:0.5 present
    CHECK(p66 < 0.7f);              // bounded ~0.5
    CHECK(diff > 0.1f);             // distinct content per bus pair
}

// C. Receive-only mode: enableLinkAudio is on for any non-Off
// visibility, so a synth gets audio without /clock/audio/publish/set.
TEST_CASE("LinkAudio: receive-only mode delivers audio to synths",
          "[Link][LinkAudio][integration]") {
    FakeLinkPeerProcess::Options peerOpts;
    peerOpts.name     = "FakeLive";
    peerOpts.channels = {{"Main", 2, "sine440-880"}};
    FakeLinkPeerProcess peer{peerOpts};
    REQUIRE(peer.ready());

    // Manual pump: reads the output bus below; test thread is the sole writer.
    auto cfg = EngineFixture::defaultConfig();
    cfg.manualAudioPump = true;
    EngineFixture fx(cfg);
    fx.send(osc_test::message("/clock/visibility", int32_t{2}));  // NetworkWide
    // NB: no /clock/audio/publish/set — engine is receive-only.

    REQUIRE(waitForChannelVisible(fx, "FakeLive", "Main",
                                   std::chrono::seconds(30)));

    {
        osc_test::Builder b;
        auto& s = b.begin("/clock/audio/input/add");
        s << "FakeLive" << "Main" << static_cast<int32_t>(64);
        fx.clearReplies();
        fx.send(b.end());
    }
    OscReply addReply;
    REQUIRE(fx.waitForReply("/clock/audio/input/add.reply", addReply, 1000));
    REQUIRE(addReply.parsed().argInt(0) == 1);
    REQUIRE(waitForConnected(fx, "FakeLive", "Main",
                              std::chrono::seconds(30)).state
            == kStateConnected);

    // Stereo passthrough — bus 64/65 → output 0/1, preserving L/R.
    REQUIRE(fx.loadSynthDef("stereo_passthrough"));
    {
        osc_test::Builder b;
        auto& s = b.begin("/s_new");
        s << "stereo_passthrough" << static_cast<int32_t>(1000)
          << static_cast<int32_t>(0) << static_cast<int32_t>(1)
          << "in_bus" << 64.0f << "out" << 0.0f;
        fx.send(b.end());
    }
    OscReply r;
    fx.send(osc_test::message("/sync", 42));
    REQUIRE(fx.waitForReply("/synced", r));

    // Poll the output bus — same rationale as Test A.
    const auto* outBus = reinterpret_cast<const float*>(get_audio_output_bus());
    REQUIRE(outBus != nullptr);
    constexpr uint32_t kBlockSize = 128;
    float pL = 0.0f, pR = 0.0f, diff = 0.0f;
    fx.pollUntil([&] {
        std::vector<float> outL(outBus,             outBus +     kBlockSize);
        std::vector<float> outR(outBus + kBlockSize, outBus + 2 * kBlockSize);
        pL   = peakAbs(outL);
        pR   = peakAbs(outR);
        diff = maxAbsDiff(outL, outR);
        return pL > 0.01f && pR > 0.01f && diff > 0.01f;
    }, 5000);
    INFO("receive-only output peakL=" << pL << " peakR=" << pR
         << " maxAbsDiff=" << diff);
    CHECK(pL > 0.01f);
    CHECK(pR > 0.01f);
    CHECK(diff > 0.01f);  // stereo preserved without publish enabled
}

// D. /clock/audio/input/remove clears the sub and stops drain from
// touching the bus pair. Private buses retain their last value, so
// we check the bus stops being REFRESHED rather than that it's zero.
TEST_CASE("LinkAudio: /clock/audio/input/remove silences the bus",
          "[Link][LinkAudio][integration]") {
    FakeLinkPeerProcess::Options peerOpts;
    peerOpts.name     = "FakeLive";
    peerOpts.channels = {{"Main", 2, "sine440-880"}};
    FakeLinkPeerProcess peer{peerOpts};
    REQUIRE(peer.ready());

    // Manual pump: this test snapshots bus 64 (before and after removal), so the
    // test thread must own the audio thread — no real-time driver writing the bus.
    auto cfg = EngineFixture::defaultConfig();
    cfg.manualAudioPump = true;
    EngineFixture fx(cfg);
    fx.send(osc_test::message("/clock/visibility",        int32_t{2}));
    fx.send(osc_test::message("/clock/audio/publish/set", int32_t{1}));

    REQUIRE(waitForChannelVisible(fx, "FakeLive", "Main",
                                   std::chrono::seconds(30)));
    {
        osc_test::Builder b;
        auto& s = b.begin("/clock/audio/input/add");
        s << "FakeLive" << "Main" << static_cast<int32_t>(64);
        fx.clearReplies();
        fx.send(b.end());
    }
    OscReply addReply;
    REQUIRE(fx.waitForReply("/clock/audio/input/add.reply", addReply, 1000));
    REQUIRE(addReply.parsed().argInt(0) == 1);
    REQUIRE(waitForConnected(fx, "FakeLive", "Main",
                              std::chrono::seconds(30)).state
            == kStateConnected);

    // Confirm audio is on the bus before removal — pollUntil() pumps a block on
    // this thread before each check, so the snapshot can't race the drain.
    constexpr uint32_t kBlockSize = 128;
    std::vector<float> busL;
    REQUIRE(fx.pollUntil([&] {
        REQUIRE(snapshotBus(64, kBlockSize, busL));
        return peakAbs(busL) > 0.01f;
    }, 5000));

    // Explicit remove.
    {
        osc_test::Builder b;
        auto& s = b.begin("/clock/audio/input/remove");
        s << "FakeLive" << "Main";
        fx.send(b.end());
    }
    CHECK(fx.pollUntil([&] { return countInputs(fx) == 0; }));

    // After remove + a few audio blocks, drainLinkAudioInputsToBuses
    // no longer writes the pair — and nothing else is writing here,
    // so the bus settles. (May still contain the last sample frozen
    // in time on the private region; we verify it stops *changing*.)
    // Pump the drain a few times with the sub now removed — it must not touch
    // bus 64 — then confirm two snapshots are identical. All on this thread, so
    // the comparison is exact and race-free.
    std::vector<float> snap1, snap2;
    fx.pumpBlock(4);
    REQUIRE(snapshotBus(64, kBlockSize, snap1));
    fx.pumpBlock(4);
    REQUIRE(snapshotBus(64, kBlockSize, snap2));
    INFO("post-remove drift=" << maxAbsDiff(snap1, snap2));
    CHECK(maxAbsDiff(snap1, snap2) < 1e-6f);
}

// Zero queue-drops at the default 50 ms lookahead across the full
// Link BPM range (20..999) and across realistic peer block sizes.
TEST_CASE("LinkAudio: no drops at default lookahead across BPM + block-size",
          "[Link][LinkAudio][integration]") {
    auto probe = [](double bpm, const char* label,
                     std::chrono::seconds duration,
                     int peerBlockSize = 1024) {
        FakeLinkPeerProcess::Options peerOpts;
        peerOpts.name      = "FakeLive";
        peerOpts.blockSize = peerBlockSize;
        peerOpts.sampleRate = 48000;
        peerOpts.channels  = {{"Main", 2, "sine440-880"}};
        FakeLinkPeerProcess peer{peerOpts};
        REQUIRE(peer.ready());

        EngineFixture fx;
        fx.send(osc_test::message("/clock/visibility",        int32_t{2}));
        fx.send(osc_test::message("/clock/audio/publish/set", int32_t{1}));
        fx.send(osc_test::message("/clock/tempo/set", static_cast<float>(bpm)));

        REQUIRE(waitForChannelVisible(fx, "FakeLive", "Main",
                                       std::chrono::seconds(30)));
        {
            osc_test::Builder b;
            auto& s = b.begin("/clock/audio/input/add");
            s << "FakeLive" << "Main" << static_cast<int32_t>(64);
            fx.clearReplies();
            fx.send(b.end());
        }
        OscReply addReply;
        REQUIRE(fx.waitForReply("/clock/audio/input/add.reply", addReply, 1000));
        REQUIRE(addReply.parsed().argInt(0) == 1);
        REQUIRE(waitForConnected(fx, "FakeLive", "Main",
                                  std::chrono::seconds(30)).state
                == kStateConnected);

        std::this_thread::sleep_for(duration);

        fx.clearReplies();
        fx.send(osc_test::message("/clock/audio/inputs/get"));
        OscReply reply;
        REQUIRE(fx.waitForReply("/clock/audio/inputs.reply", reply, 500));
        const auto p = reply.parsed();
        const int count = p.argInt(0);
        struct Stats {
            uint64_t queueDrops{0};
            uint64_t networkGaps{0};
            uint64_t totalCalls{0};
            uint64_t duplicates{0};
        } st;
        for (int i = 0; i < count; ++i) {
            const int base = 1 + i * 12;
            if (p.argString(base + 0) == "FakeLive" &&
                p.argString(base + 1) == "Main") {
                st.queueDrops  = static_cast<uint64_t>(p.argInt(base + 7));
                st.networkGaps = static_cast<uint64_t>(p.argInt(base + 8));
                st.totalCalls  = static_cast<uint64_t>(p.argInt(base + 9));
                st.duplicates  = static_cast<uint64_t>(p.argInt(base + 10));
                break;
            }
        }

        const double durationSec = static_cast<double>(duration.count());
        std::fprintf(stderr,
            "[queue-depth-probe %s] bpm=%.1f peerBlock=%d duration=%.0fs  "
            "totalCalls=%llu (%.1f/s)  queueDrops=%llu (%.1f/s)  "
            "duplicates=%llu  networkGaps=%llu\n",
            label, bpm, peerBlockSize, durationSec,
            static_cast<unsigned long long>(st.totalCalls),
            st.totalCalls / durationSec,
            static_cast<unsigned long long>(st.queueDrops),
            st.queueDrops / durationSec,
            static_cast<unsigned long long>(st.duplicates),
            static_cast<unsigned long long>(st.networkGaps));
        return st;
    };

    // Three points spanning Link's BPM clamp range (20..999).
    const auto sHigh   = probe(999.0, "HIGH  (999 bpm)", std::chrono::seconds(3));
    const auto sMiddle = probe(120.0, "MID   (120 bpm)", std::chrono::seconds(3));
    const auto sLow    = probe(20.0,  "LOW   (20 bpm)",  std::chrono::seconds(3));

    // Block-size sweep at fixed BPM — chunk rate is set by Link's
    // per-chunk byte cap, not the peer's block size.
    std::fprintf(stderr, "\n--- block-size sweep at 120 BPM ---\n");
    const auto b64   = probe(120.0, "block=64",   std::chrono::seconds(2), 64);
    const auto b256  = probe(120.0, "block=256",  std::chrono::seconds(2), 256);
    const auto b2048 = probe(120.0, "block=2048", std::chrono::seconds(2), 2048);

    INFO("HIGH calls=" << sHigh.totalCalls << " drops=" << sHigh.queueDrops
         << " | MID calls=" << sMiddle.totalCalls << " drops=" << sMiddle.queueDrops
         << " | LOW calls="  << sLow.totalCalls  << " drops=" << sLow.queueDrops
         << " | b64 drops=" << b64.queueDrops
         << " | b256 drops=" << b256.queueDrops
         << " | b2048 drops=" << b2048.queueDrops);

    // Loopback peer has no network path to drop on.
    CHECK(sHigh.networkGaps   == 0);
    CHECK(sMiddle.networkGaps == 0);
    CHECK(sLow.networkGaps    == 0);

    CHECK(sHigh.queueDrops   == 0);
    CHECK(sMiddle.queueDrops == 0);
    CHECK(sLow.queueDrops    == 0);
    CHECK(b64.queueDrops     == 0);
    CHECK(b256.queueDrops    == 0);
    CHECK(b2048.queueDrops   == 0);
}

// Re-adding the same (peer, channel) reuses the existing renderer
// so lifetime diagnostic counters (drops, gaps, totalCalls) survive
// across re-arms.
TEST_CASE("LinkAudio: replacement preserves renderer diagnostic counters",
          "[Link][LinkAudio][integration]") {
    FakeLinkPeerProcess::Options peerOpts;
    peerOpts.name     = "FakeLive";
    peerOpts.channels = {{"Main", 2, "sine440-880"}};
    FakeLinkPeerProcess peer{peerOpts};
    REQUIRE(peer.ready());

    EngineFixture fx;
    fx.send(osc_test::message("/clock/visibility",        int32_t{2}));
    fx.send(osc_test::message("/clock/audio/publish/set", int32_t{1}));
    REQUIRE(waitForChannelVisible(fx, "FakeLive", "Main",
                                   std::chrono::seconds(30)));

    auto sendAdd = [&](int32_t busIdx) {
        osc_test::Builder b;
        auto& s = b.begin("/clock/audio/input/add");
        s << "FakeLive" << "Main" << busIdx;
        fx.clearReplies();
        fx.send(b.end());
        OscReply r;
        REQUIRE(fx.waitForReply("/clock/audio/input/add.reply", r, 1000));
        return r.parsed().argInt(0);
    };
    REQUIRE(sendAdd(64) == 1);
    // Wait for the renderer to be Connected, then accumulate calls.
    REQUIRE(waitForConnected(fx, "FakeLive", "Main",
                              std::chrono::seconds(30)).state
            == kStateConnected);
    std::this_thread::sleep_for(std::chrono::seconds(2));

    auto readTotalCalls = [&]() -> uint64_t {
        fx.clearReplies();
        fx.send(osc_test::message("/clock/audio/inputs/get"));
        OscReply r;
        REQUIRE(fx.waitForReply("/clock/audio/inputs.reply", r, 500));
        const auto p = r.parsed();
        REQUIRE(p.argInt(0) == 1);
        return static_cast<uint64_t>(p.argInt(1 + 9));  // totalSourceBufferCalls
    };
    const uint64_t before = readTotalCalls();
    REQUIRE(before > 0);  // confirm we're receiving

    // Re-add same (peer, channel, busIdx). Should be a no-op vs the
    // existing renderer — counters must NOT reset.
    REQUIRE(sendAdd(64) == 1);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    const uint64_t after = readTotalCalls();
    INFO("before=" << before << " after=" << after);
    CHECK(after >= before);  // preserved (possibly grew further)
}

// Latency above the renderer's queue capacity (kMaxLinkAudioInputLatencySeconds)
// must be rejected by the OSC setter so callers see an error rather
// than discovering it as silent dropouts.
TEST_CASE("LinkAudio: latency setter rejects values above the supported max",
          "[Link][LinkAudio][integration]") {
    FakeLinkPeerProcess::Options peerOpts;
    peerOpts.name     = "FakeLive";
    peerOpts.channels = {{"Main", 2, "sine440-880"}};
    FakeLinkPeerProcess peer{peerOpts};
    REQUIRE(peer.ready());

    EngineFixture fx;
    fx.send(osc_test::message("/clock/visibility",        int32_t{2}));
    fx.send(osc_test::message("/clock/audio/publish/set", int32_t{1}));
    REQUIRE(waitForChannelVisible(fx, "FakeLive", "Main",
                                   std::chrono::seconds(30)));
    {
        osc_test::Builder b;
        auto& s = b.begin("/clock/audio/input/add");
        s << "FakeLive" << "Main" << static_cast<int32_t>(64);
        fx.clearReplies();
        fx.send(b.end());
    }
    OscReply addReply;
    REQUIRE(fx.waitForReply("/clock/audio/input/add.reply", addReply, 1000));
    REQUIRE(addReply.parsed().argInt(0) == 1);

    auto setLatency = [&](float seconds) {
        osc_test::Builder b;
        auto& s = b.begin("/clock/audio/input/latency/set");
        s << "FakeLive" << "Main" << seconds;
        fx.clearReplies();
        fx.send(b.end());
        OscReply r;
        REQUIRE(fx.waitForReply("/clock/audio/input/latency/set.reply", r, 1000));
        return r.parsed().argInt(0);
    };

    // Within range: accepted.
    CHECK(setLatency(0.05f) == 1);
    CHECK(setLatency(2.0f)  == 1);

    // Above Live's 2 s ceiling: must reject, not silently accept and
    // start dropping. Caller should see success=0.
    CHECK(setLatency(2.5f)  == 0);
    CHECK(setLatency(10.0f) == 0);
    CHECK(setLatency(1e9f)  == 0);
}

// /clock/audio/input/latency/set succeeds, the new value is reflected
// in /clock/audio/inputs/get, and the renderer handles a high (1.5 s)
// lookahead with zero drops.
TEST_CASE("LinkAudio: per-input latency setter takes effect end-to-end",
          "[Link][LinkAudio][integration]") {
    FakeLinkPeerProcess::Options peerOpts;
    peerOpts.name     = "FakeLive";
    peerOpts.channels = {{"Main", 2, "sine440-880"}};
    FakeLinkPeerProcess peer{peerOpts};
    REQUIRE(peer.ready());

    EngineFixture fx;
    fx.send(osc_test::message("/clock/visibility",        int32_t{2}));
    fx.send(osc_test::message("/clock/audio/publish/set", int32_t{1}));

    REQUIRE(waitForChannelVisible(fx, "FakeLive", "Main",
                                   std::chrono::seconds(30)));
    {
        osc_test::Builder b;
        auto& s = b.begin("/clock/audio/input/add");
        s << "FakeLive" << "Main" << static_cast<int32_t>(64);
        fx.clearReplies();
        fx.send(b.end());
    }
    OscReply addReply;
    REQUIRE(fx.waitForReply("/clock/audio/input/add.reply", addReply, 1000));
    REQUIRE(addReply.parsed().argInt(0) == 1);
    REQUIRE(waitForConnected(fx, "FakeLive", "Main",
                              std::chrono::seconds(30)).state
            == kStateConnected);

    // Set lookahead to 1.5 s. Renderer ring is depth 2048 → covers
    // ~2.6 s at Link's wire rate, so 1.5 s should drop zero.
    {
        osc_test::Builder b;
        auto& s = b.begin("/clock/audio/input/latency/set");
        s << "FakeLive" << "Main" << 1.5f;
        fx.clearReplies();
        fx.send(b.end());
    }
    OscReply setReply;
    REQUIRE(fx.waitForReply("/clock/audio/input/latency/set.reply", setReply, 1000));
    CHECK(setReply.parsed().argInt(0) == 1);

    // Let the consumer build out the deeper retained set, then check
    // the reply field reflects the new value and that no drops occurred.
    std::this_thread::sleep_for(std::chrono::seconds(3));

    fx.clearReplies();
    fx.send(osc_test::message("/clock/audio/inputs/get"));
    OscReply inputsReply;
    REQUIRE(fx.waitForReply("/clock/audio/inputs.reply", inputsReply, 500));
    const auto p = inputsReply.parsed();
    REQUIRE(p.argInt(0) >= 1);
    const int base = 1 + 0 * 12;  // first (only) entry
    CHECK(p.argString(base + 0) == "FakeLive");
    CHECK(p.argString(base + 1) == "Main");
    CHECK(std::fabs(p.argFloat(base + 11) - 1.5f) < 0.001f);
    // No drops in the steady-state with a 1.5 s window — the ring is
    // sized for it. networkGaps stays zero (loopback peer).
    CHECK(p.argInt(base + 7) == 0);   // droppedSourceBuffers
    CHECK(p.argInt(base + 8) == 0);   // networkGapBuffers
}

#endif  // SUPERSONIC_LINK
