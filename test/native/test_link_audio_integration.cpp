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
//
// PEER AUDIO ARRIVES ON AN INPUT CHANNEL THE ENGINE CHOOSES. A subscription
// used to name a bus in the DSP's private pool, and the bridge wrote there
// directly; then it named an input channel of the client's choosing. Now the
// engine hands it a pair of its Link Audio lanes, above every device channel,
// and the reply says which (subscribe, below).

//
// LinkAudioBridge.h FIRST: it is what defines CLOCKWORK_LINK_AUDIO, to 0 or 1.
// Tested before it was included, the macro was never defined, and this whole
// file compiled to nothing in every Link Audio build, from Clockwork's
// extraction until October 2026.
#include "native/LinkAudioBridge.h"
#if CLOCKWORK_LINK_AUDIO

#include "EngineFixture.h"
#include "FakeLinkPeerProcess.h"
#include "ClockworkProcessor.h"
#include "OscTestUtils.h"
#include "lanes/lanes.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <memory>
#include <thread>
#include <utility>
#include <vector>

namespace {

// Sanitizer builds (ASAN/TSAN) run ~2-3x slower, so a fixed wall-clock wait
// for out-of-process peer discovery / buffer fill can expire before the work
// completes — a false failure that means "the sanitized build was slow", not
// "it never happened". Scale such waits up under a sanitizer build only.
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
constexpr int kTimeoutScale = 3;
#elif defined(__has_feature)
#  if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer)
constexpr int kTimeoutScale = 3;
#  else
constexpr int kTimeoutScale = 1;
#  endif
#else
constexpr int kTimeoutScale = 1;
#endif

// Connection-state enum mirrors ClockworkClock::LinkAudioConnectionState.
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
    timeout *= kTimeoutScale;  // sanitizer builds run slower; scale the wait
    const auto deadline = clock::now() + timeout;
    while (clock::now() < deadline) {
        fx.clearReplies();
        fx.send(osc_test::message("/clockwork/clock/audio/channels/get"));
        OscReply reply;
        if (fx.waitForReply("/clockwork/clock/audio/channels.reply", reply, 200)) {
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

// /clockwork/clock/audio/inputs.reply is <count>, then these for each input.
enum InputField {
    kPeer, kChannel, kBus, kRate, kSourceChannels, kBufferedMs, kState,
    kDropped, kNetworkGaps, kSourceCalls, kDuplicates, kLatency,
    kUnderruns, kResyncs, kWarps, kDriftPpm,
    kInputFields
};

// One input's line of the report; state -1 when the engine has no such input.
struct InputSnapshot {
    int      state = -1;
    int      sourceNumChannels = 0;
    float    bufferedMs = 0.0f;
    uint64_t dropped = 0, networkGaps = 0, sourceCalls = 0, duplicates = 0;
    float    latencySeconds = 0.0f;
    uint64_t underruns = 0, resyncs = 0, warps = 0;
    int      driftPpm = 0;
};

InputSnapshot snapshotInput(EngineFixture& fx,
                            const std::string& peerName,
                            const std::string& channelName) {
    InputSnapshot snap;
    fx.clearReplies();
    fx.send(osc_test::message("/clockwork/clock/audio/inputs/get"));
    OscReply reply;
    if (!fx.waitForReply("/clockwork/clock/audio/inputs.reply", reply, 500)) return snap;
    const auto p = reply.parsed();
    const int count = p.argInt(0);
    for (int i = 0; i < count; ++i) {
        const int base = 1 + i * kInputFields;
        if (p.argString(base + kPeer) != peerName || p.argString(base + kChannel) != channelName)
            continue;
        auto count64 = [&](int field) { return static_cast<uint64_t>(p.argInt(base + field)); };
        snap.sourceNumChannels = p.argInt(base + kSourceChannels);
        snap.bufferedMs        = p.argFloat(base + kBufferedMs);
        snap.state             = p.argInt(base + kState);
        snap.dropped           = count64(kDropped);
        snap.networkGaps       = count64(kNetworkGaps);
        snap.sourceCalls       = count64(kSourceCalls);
        snap.duplicates        = count64(kDuplicates);
        snap.latencySeconds    = p.argFloat(base + kLatency);
        snap.underruns         = count64(kUnderruns);
        snap.resyncs           = count64(kResyncs);
        snap.warps             = count64(kWarps);
        snap.driftPpm          = p.argInt(base + kDriftPpm);
        break;
    }
    return snap;
}

// Subscribes the peer's channel and answers the input channel the engine put
// it on, or -1 when it refused. The engine chooses: the first pair of its Link
// Audio lanes (clockwork_link_audio_lane_base) that no other subscription
// holds.
int32_t subscribe(EngineFixture& fx, const char* peer, const char* channel) {
    osc_test::Builder b;
    auto& s = b.begin("/clockwork/clock/audio/input/add");
    s << peer << channel;
    fx.clearReplies();
    fx.send(b.end());
    OscReply r;
    REQUIRE(fx.waitForReply("/clockwork/clock/audio/input/add.reply", r, 1000));
    const auto p = r.parsed();
    return p.argInt(0) == 1 ? p.argInt(1) : -1;
}

// IN.AR TAKES A SCSYNTH BUS, NOT A CLOCKWORK CHANNEL, and they are not the
// same number. scsynth lays its bus pool out as outputs first, then inputs
// (scsynth_dsp.cpp: mAudioBus + mNumOutputs * mBufLength), so clockwork's
// input channel N is bus mNumOutputs + N to a synth. The lanes widen the
// outputs as well as the inputs, so mNumOutputs is the allocated count, not
// the device's.
float inBusFor(int32_t channel) {
    return static_cast<float>(get_audio_num_output_buses() + channel);
}

// Six input channels, as a device might have. A stream never lands on them:
// the engine puts it in its Link Audio lanes, above every device channel.
ClockworkEngine::Config linkInputConfig() {
    auto cfg = EngineFixture::defaultConfig();
    cfg.numInputChannels = 6;
    return cfg;
}

}  // namespace

TEST_CASE("LinkAudio: receives audio from peer with 1024-frame buffers",
          "[Link][LinkAudio][integration]") {
    FakeLinkPeerProcess::Options peerOpts;
    peerOpts.name         = "FakeLive";
    peerOpts.blockSize    = 1024;   // matches Live's typical engine size
    peerOpts.sampleRate   = 48000;
    peerOpts.channels     = {{"Main", 2, "sine440-880"}};
    FakeLinkPeerProcess peer{peerOpts};
    REQUIRE(peer.ready());

    EngineFixture fx{linkInputConfig()};
    // NetworkWide + publish=1 so LinkAudio is on and the engine can
    // see other peers' channels via link.channels().
    fx.send(osc_test::message("/clockwork/clock/visibility",         int32_t{2}));
    fx.send(osc_test::message("/clockwork/clock/audio/publish/set",  int32_t{1}));

    REQUIRE(waitForChannelVisible(fx, "FakeLive", "Main",
                                   std::chrono::seconds(30)));

    const int32_t main = subscribe(fx, "FakeLive", "Main");
    REQUIRE(main >= 0);

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
    fx.send(osc_test::message("/clockwork/clock/audio/inputs/get"));
    OscReply reply;
    if (!fx.waitForReply("/clockwork/clock/audio/inputs.reply", reply, 500)) return -1;
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

    EngineFixture fx{linkInputConfig()};
    fx.send(osc_test::message("/clockwork/clock/visibility",        int32_t{2}));
    fx.send(osc_test::message("/clockwork/clock/audio/publish/set", int32_t{1}));

    REQUIRE(waitForChannelVisible(fx, "FakeLive", "Main",
                                   std::chrono::seconds(30)));

    const int32_t main = subscribe(fx, "FakeLive", "Main");
    REQUIRE(main >= 0);
    REQUIRE(countInputs(fx) == 1);

    fx.send(osc_test::message("/clockwork/clock/reset"));
    // Poll until the async reset has cleared the subscription, rather than
    // guessing a fixed delay a loaded runner may overrun.
    CHECK(fx.pollUntil([&] { return countInputs(fx) == 0; }));
}

// ── Where a subscription lands ─────────────────────────────────────────────
//
// THE ENGINE CHOOSES, and the reply says where. A peer's audio arrives on a
// pair of the engine's Link Audio lanes, which sit above every device channel
// and every host lane, so a stream can never overwrite a microphone or a
// track's return. A client used to pass an index of its own, and when that
// index changed meaning (a private bus, then an input channel) a client that
// kept passing the old kind was refused on every subscription without either
// side's tests noticing. There is nothing left for a client to get wrong.

namespace {

// A peer publishing `count` mono channels, C0, C1 ... so a test can ask for
// more subscriptions than the lanes hold.
FakeLinkPeerProcess::Options peerWithChannels(int count) {
    FakeLinkPeerProcess::Options opts;
    opts.name = "FakeLive";
    for (int i = 0; i < count; ++i)
        opts.channels.push_back({"C" + std::to_string(i), 1, "dc:0.25"});
    return opts;
}

void joinMesh(EngineFixture& fx, const FakeLinkPeerProcess::Options& peer) {
    fx.send(osc_test::message("/clockwork/clock/visibility",        int32_t{2}));
    fx.send(osc_test::message("/clockwork/clock/audio/publish/set", int32_t{1}));
    for (const auto& c : peer.channels)
        REQUIRE(waitForChannelVisible(fx, peer.name, c.name, std::chrono::seconds(30)));
}

}  // namespace

TEST_CASE("LinkAudio: each subscription gets a pair of the engine's Link Audio lanes",
          "[Link][LinkAudio][integration]") {
    const auto peerOpts = peerWithChannels(2);
    FakeLinkPeerProcess peer{peerOpts};
    REQUIRE(peer.ready());
    EngineFixture fx{linkInputConfig()};
    joinMesh(fx, peerOpts);

    const auto base = static_cast<int32_t>(clockwork_link_audio_lane_base());
    REQUIRE(base >= static_cast<int32_t>(linkInputConfig().numInputChannels));
    CHECK(subscribe(fx, "FakeLive", "C0") == base);
    CHECK(subscribe(fx, "FakeLive", "C1") == base + 2);

    // The same stream again keeps its pair: a client re-triggering it reads
    // it where it already was.
    CHECK(subscribe(fx, "FakeLive", "C0") == base);
}

TEST_CASE("LinkAudio: a removed subscription's pair goes to the next",
          "[Link][LinkAudio][integration]") {
    const auto peerOpts = peerWithChannels(3);
    FakeLinkPeerProcess peer{peerOpts};
    REQUIRE(peer.ready());
    EngineFixture fx{linkInputConfig()};
    joinMesh(fx, peerOpts);

    const int32_t c0 = subscribe(fx, "FakeLive", "C0");
    const int32_t c1 = subscribe(fx, "FakeLive", "C1");
    REQUIRE(c0 >= 0);
    REQUIRE(c1 >= 0);
    {
        osc_test::Builder b;
        auto& s = b.begin("/clockwork/clock/audio/input/remove");
        s << "FakeLive" << "C0";
        fx.send(b.end());
    }
    REQUIRE(fx.pollUntil([&] { return countInputs(fx) == 1; }));

    // Streams leaving do not shift the ones that stay, and the gap is
    // filled first.
    CHECK(subscribe(fx, "FakeLive", "C2") == c0);
    CHECK(snapshotInput(fx, "FakeLive", "C1").state != kStateNotSubscribed);
}

TEST_CASE("LinkAudio: a subscription the lanes have no room for is refused",
          "[Link][LinkAudio][integration]") {
    // Refused rather than accepted: a stream bound past the lanes would be
    // skipped by pull_port_sources and look exactly like a working
    // subscription that is silent for ever.
    // The constant, not clockwork_link_audio_lanes(): that is 0 until an
    // engine has booted, and the peer has to exist first.
    const auto pairs = static_cast<int>(LinkAudioBridge::kLanes / 2);
    const auto peerOpts = peerWithChannels(pairs + 1);
    FakeLinkPeerProcess peer{peerOpts};
    REQUIRE(peer.ready());
    EngineFixture fx{linkInputConfig()};
    joinMesh(fx, peerOpts);
    REQUIRE(clockwork_link_audio_lanes() == LinkAudioBridge::kLanes);

    for (int i = 0; i < pairs; ++i)
        REQUIRE(subscribe(fx, "FakeLive", ("C" + std::to_string(i)).c_str()) >= 0);
    CHECK(subscribe(fx, "FakeLive", ("C" + std::to_string(pairs)).c_str()) == -1);
}

TEST_CASE("LinkAudio: the input and channel replies echo a correlation token",
          "[Link][LinkAudio][integration]") {
    // A request's trailing int32 comes back as its reply's last argument, as
    // on every other /clockwork/clock reply, so a client matching replies by
    // token (Sonic Pi's rpc) can read these at all.
    const auto peerOpts = peerWithChannels(1);
    FakeLinkPeerProcess peer{peerOpts};
    REQUIRE(peer.ready());
    EngineFixture fx{linkInputConfig()};
    joinMesh(fx, peerOpts);

    auto lastArg = [&](osc_test::Builder& b, const char* reply) {
        fx.clearReplies();
        fx.send(b.end());
        OscReply r;
        REQUIRE(fx.waitForReply(reply, r, 1000));
        const auto p = r.parsed();
        return p.argInt(static_cast<int>(p.argCount()) - 1);
    };
    {
        osc_test::Builder b;
        b.begin("/clockwork/clock/audio/input/add") << "FakeLive" << "C0" << int32_t{4242};
        CHECK(lastArg(b, "/clockwork/clock/audio/input/add.reply") == 4242);
    }
    {
        osc_test::Builder b;
        b.begin("/clockwork/clock/audio/inputs/get") << int32_t{4343};
        CHECK(lastArg(b, "/clockwork/clock/audio/inputs.reply") == 4343);
    }
    {
        osc_test::Builder b;
        b.begin("/clockwork/clock/audio/channels/get") << int32_t{4444};
        CHECK(lastArg(b, "/clockwork/clock/audio/channels.reply") == 4444);
    }
}

TEST_CASE("LinkAudio: setLinkVisibility(Off) clears active input subscriptions",
          "[Link][LinkAudio][integration]") {
    FakeLinkPeerProcess::Options peerOpts;
    peerOpts.name       = "FakeLive";
    peerOpts.channels   = {{"Main", 2, "sine440-880"}};
    FakeLinkPeerProcess peer{peerOpts};
    REQUIRE(peer.ready());

    EngineFixture fx{linkInputConfig()};
    fx.send(osc_test::message("/clockwork/clock/visibility",        int32_t{2}));
    fx.send(osc_test::message("/clockwork/clock/audio/publish/set", int32_t{1}));

    REQUIRE(waitForChannelVisible(fx, "FakeLive", "Main",
                                   std::chrono::seconds(30)));

    const int32_t main = subscribe(fx, "FakeLive", "Main");
    REQUIRE(main >= 0);
    REQUIRE(countInputs(fx) == 1);

    fx.send(osc_test::message("/clockwork/clock/visibility", int32_t{0}));  // Off
    CHECK(fx.pollUntil([&] { return countInputs(fx) == 0; }));
}

namespace {

// Read `blockSize` samples from one INPUT CHANNEL into `out`.
//
// It used to read the DSP's private bus pool through get_audio_bus_pool(), an
// accessor the seam narrowing removed — clockwork does not hand out the
// guest's signal storage. Peer audio arrives as ordinary input channels now
// (a source port bound through clockwork_port_bus.h), so this reads the input bus
// clockwork does expose. No snapshot, so a torn-mid-write read is possible;
// for amplitude and L!=R assertions the race does not change the outcome.
bool snapshotBus(uint32_t chIdx, uint32_t blockSize, std::vector<float>& out) {
    const auto* in = reinterpret_cast<const float*>(get_audio_input_bus());
    if (!in) return false;
    if (chIdx >= static_cast<uint32_t>(get_audio_num_input_buses())) return false;
    out.assign(in + chIdx * blockSize, in + (chIdx + 1) * blockSize);
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

// Wait until `peerName`/`channelName` has audio arriving, healthy or not.
// What a hand-pumped engine can wait for: pumped a block at a time between
// polls, slower than a device would, it hears a stream it cannot keep up
// with, and the report says so. Its health is for a device-paced listen.
bool waitForArriving(EngineFixture& fx,
                     const std::string& peerName,
                     const std::string& channelName,
                     std::chrono::milliseconds timeout) {
    return fx.pollUntil([&] {
        const int state = snapshotInput(fx, peerName, channelName).state;
        return state == kStateConnected || state == kStateDropout;
    }, static_cast<int>(timeout.count()));
}

}  // namespace

// A. Verify scsynth's audio graph actually consumes Link-delivered
// audio (via In.ar) — bus contents alone would only prove
// drainLinkAudioInputsToBuses wrote bytes somewhere.
// Path: FakeLive → the subscription's input pair → stereo_passthrough
// (In.ar(inBusFor(pair), 2) → Out.ar(0,_)) → engine output bus 0/1.
TEST_CASE("LinkAudio: scsynth In.ar consumes audio from a Link subscription",
          "[Link][LinkAudio][integration]") {
    FakeLinkPeerProcess::Options peerOpts;
    peerOpts.name     = "FakeLive";
    peerOpts.channels = {{"Main", 2, "sine440-880"}};
    FakeLinkPeerProcess peer{peerOpts};
    REQUIRE(peer.ready());

    // Manual pump: this test reads the output bus, so the test thread must be the
    // sole audio-thread writer (no real-time driver) or the read races the drain.
    auto cfg = linkInputConfig();
    cfg.manualAudioPump = true;
    EngineFixture fx(cfg);
    fx.send(osc_test::message("/clockwork/clock/visibility",        int32_t{2}));
    fx.send(osc_test::message("/clockwork/clock/audio/publish/set", int32_t{1}));

    REQUIRE(waitForChannelVisible(fx, "FakeLive", "Main",
                                   std::chrono::seconds(30)));

    const int32_t main = subscribe(fx, "FakeLive", "Main");
    REQUIRE(main >= 0);
    REQUIRE(waitForArriving(fx, "FakeLive", "Main", std::chrono::seconds(30)));

    // The synth reads the scsynth bus the stream's input channel is
    // (inBusFor). stereo_passthrough reads In.ar(in_bus, 2) and writes both
    // channels unchanged to (out, out+1). That preserves the peer's
    // L/R distinction end-to-end so we can assert real stereo here.
    REQUIRE(fx.loadSynthDef("stereo_passthrough"));
    {
        osc_test::Builder b;
        auto& s = b.begin("/s_new");
        s << "stereo_passthrough" << static_cast<int32_t>(1000)
          << static_cast<int32_t>(0) << static_cast<int32_t>(1)
          << "in_bus" << inBusFor(main) << "out" << 0.0f;
        fx.send(b.end());
    }
    {
        OscReply r;
        fx.send(osc_test::message("/sync", 42));
        REQUIRE(fx.waitForReply("/synced", r));
    }

    // Poll the output bus — slow CI runners can take longer than a
    // fixed sleep to: (1) let the HeadlessDriver drain Link audio
    // into its pair, (2) let scsynth run the synth at least once with
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
    auto cfg = linkInputConfig();
    cfg.manualAudioPump = true;
    EngineFixture fx(cfg);
    fx.send(osc_test::message("/clockwork/clock/visibility",        int32_t{2}));
    fx.send(osc_test::message("/clockwork/clock/audio/publish/set", int32_t{1}));

    REQUIRE(waitForChannelVisible(fx, "FakeLive", "ChanA",
                                   std::chrono::seconds(30)));
    REQUIRE(waitForChannelVisible(fx, "FakeLive", "ChanB",
                                   std::chrono::seconds(5)));

    const int32_t chanA = subscribe(fx, "FakeLive", "ChanA");
    const int32_t chanB = subscribe(fx, "FakeLive", "ChanB");
    REQUIRE(chanA >= 0);
    REQUIRE(chanB >= 0);
    REQUIRE(chanA != chanB);

    REQUIRE(waitForArriving(fx, "FakeLive", "ChanA", std::chrono::seconds(30)));
    REQUIRE(waitForArriving(fx, "FakeLive", "ChanB", std::chrono::seconds(30)));

    // Poll bus contents — slow CI runners need time for drain to
    // populate both bus pairs after Connected. ChanA carries sine440
    // (peak near 1.0), ChanB carries dc:0.5 (peak ~0.5).
    constexpr uint32_t kBlockSize = 128;
    std::vector<float> busA, busB;
    float peakA = 0.0f, peakB = 0.0f, diff = 0.0f;
    fx.pollUntil([&] {
        REQUIRE(snapshotBus(static_cast<uint32_t>(chanA), kBlockSize, busA));
        REQUIRE(snapshotBus(static_cast<uint32_t>(chanB), kBlockSize, busB));
        peakA = peakAbs(busA);
        peakB = peakAbs(busB);
        diff  = maxAbsDiff(busA, busB);
        return peakA > 0.1f && peakB > 0.3f && diff > 0.1f;
    }, 5000);
    INFO("peakA(sine)=" << peakA << " peakB(dc)=" << peakB
         << " maxAbsDiff=" << diff);
    CHECK(peakA > 0.1f);            // sine present
    CHECK(peakB > 0.3f);            // dc:0.5 present
    CHECK(peakB < 0.7f);            // bounded ~0.5
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
    auto cfg = linkInputConfig();
    cfg.manualAudioPump = true;
    EngineFixture fx(cfg);
    fx.send(osc_test::message("/clockwork/clock/visibility", int32_t{2}));  // NetworkWide
    // NB: no /clock/audio/publish/set — engine is receive-only.

    REQUIRE(waitForChannelVisible(fx, "FakeLive", "Main",
                                   std::chrono::seconds(30)));

    const int32_t main = subscribe(fx, "FakeLive", "Main");
    REQUIRE(main >= 0);
    REQUIRE(waitForArriving(fx, "FakeLive", "Main", std::chrono::seconds(30)));

    // Stereo passthrough: the stream's pair to output 0/1, preserving L/R.
    REQUIRE(fx.loadSynthDef("stereo_passthrough"));
    {
        osc_test::Builder b;
        auto& s = b.begin("/s_new");
        s << "stereo_passthrough" << static_cast<int32_t>(1000)
          << static_cast<int32_t>(0) << static_cast<int32_t>(1)
          << "in_bus" << inBusFor(main) << "out" << 0.0f;
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

    // Manual pump: this test snapshots the stream's pair (before and after removal), so the
    // test thread must own the audio thread — no real-time driver writing the bus.
    auto cfg = linkInputConfig();
    cfg.manualAudioPump = true;
    EngineFixture fx(cfg);
    fx.send(osc_test::message("/clockwork/clock/visibility",        int32_t{2}));
    fx.send(osc_test::message("/clockwork/clock/audio/publish/set", int32_t{1}));

    REQUIRE(waitForChannelVisible(fx, "FakeLive", "Main",
                                   std::chrono::seconds(30)));
    const int32_t main = subscribe(fx, "FakeLive", "Main");
    REQUIRE(main >= 0);
    REQUIRE(waitForArriving(fx, "FakeLive", "Main", std::chrono::seconds(30)));

    // Confirm audio is on the bus before removal — pollUntil() pumps a block on
    // this thread before each check, so the snapshot can't race the drain.
    constexpr uint32_t kBlockSize = 128;
    std::vector<float> busL;
    REQUIRE(fx.pollUntil([&] {
        REQUIRE(snapshotBus(static_cast<uint32_t>(main), kBlockSize, busL));
        return peakAbs(busL) > 0.01f;
    }, 5000));

    // Explicit remove.
    {
        osc_test::Builder b;
        auto& s = b.begin("/clockwork/clock/audio/input/remove");
        s << "FakeLive" << "Main";
        fx.send(b.end());
    }
    CHECK(fx.pollUntil([&] { return countInputs(fx) == 0; }));

    // After remove + a few audio blocks, drainLinkAudioInputsToBuses
    // no longer writes the pair — and nothing else is writing here,
    // so the bus settles. (May still contain the last sample frozen
    // in time on the private region; we verify it stops *changing*.)
    // Pump the drain a few times with the sub now removed — it must not touch
    // its pair — then confirm two snapshots are identical. All on this thread, so
    // the comparison is exact and race-free.
    std::vector<float> snap1, snap2;
    fx.pumpBlock(4);
    REQUIRE(snapshotBus(static_cast<uint32_t>(main), kBlockSize, snap1));
    fx.pumpBlock(4);
    REQUIRE(snapshotBus(static_cast<uint32_t>(main), kBlockSize, snap2));
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

        EngineFixture fx{linkInputConfig()};
        fx.send(osc_test::message("/clockwork/clock/visibility",        int32_t{2}));
        fx.send(osc_test::message("/clockwork/clock/audio/publish/set", int32_t{1}));
        fx.send(osc_test::message("/clockwork/clock/tempo/set", static_cast<float>(bpm)));

        REQUIRE(waitForChannelVisible(fx, "FakeLive", "Main",
                                       std::chrono::seconds(30)));
        const int32_t main = subscribe(fx, "FakeLive", "Main");
        REQUIRE(main >= 0);
        REQUIRE(waitForConnected(fx, "FakeLive", "Main",
                                  std::chrono::seconds(30)).state
                == kStateConnected);

        std::this_thread::sleep_for(duration);

        const InputSnapshot in = snapshotInput(fx, "FakeLive", "Main");
        REQUIRE(in.state >= 0);
        struct Stats {
            uint64_t queueDrops{0};
            uint64_t networkGaps{0};
            uint64_t totalCalls{0};
            uint64_t duplicates{0};
        } st{in.dropped, in.networkGaps, in.sourceCalls, in.duplicates};

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
    const auto b64   = probe(120.0, "block=2",   std::chrono::seconds(2), 2);
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

    EngineFixture fx{linkInputConfig()};
    fx.send(osc_test::message("/clockwork/clock/visibility",        int32_t{2}));
    fx.send(osc_test::message("/clockwork/clock/audio/publish/set", int32_t{1}));
    REQUIRE(waitForChannelVisible(fx, "FakeLive", "Main",
                                   std::chrono::seconds(30)));

    const int32_t main = subscribe(fx, "FakeLive", "Main");
    REQUIRE(main >= 0);
    // Wait for the renderer to be Connected, then accumulate calls.
    REQUIRE(waitForConnected(fx, "FakeLive", "Main",
                              std::chrono::seconds(30)).state
            == kStateConnected);
    std::this_thread::sleep_for(std::chrono::seconds(2));

    auto readTotalCalls = [&]() -> uint64_t {
        fx.clearReplies();
        fx.send(osc_test::message("/clockwork/clock/audio/inputs/get"));
        OscReply r;
        REQUIRE(fx.waitForReply("/clockwork/clock/audio/inputs.reply", r, 500));
        const auto p = r.parsed();
        REQUIRE(p.argInt(0) == 1);
        return static_cast<uint64_t>(p.argInt(1 + 9));  // totalSourceBufferCalls
    };
    const uint64_t before = readTotalCalls();
    REQUIRE(before > 0);  // confirm we're receiving

    // Re-add the same (peer, channel). Should be a no-op vs the existing
    // renderer, on the same pair — counters must NOT reset.
    REQUIRE(subscribe(fx, "FakeLive", "Main") == main);
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

    EngineFixture fx{linkInputConfig()};
    fx.send(osc_test::message("/clockwork/clock/visibility",        int32_t{2}));
    fx.send(osc_test::message("/clockwork/clock/audio/publish/set", int32_t{1}));
    REQUIRE(waitForChannelVisible(fx, "FakeLive", "Main",
                                   std::chrono::seconds(30)));
    const int32_t main = subscribe(fx, "FakeLive", "Main");
    REQUIRE(main >= 0);

    auto setLatency = [&](float seconds) {
        osc_test::Builder b;
        auto& s = b.begin("/clockwork/clock/audio/input/latency/set");
        s << "FakeLive" << "Main" << seconds;
        fx.clearReplies();
        fx.send(b.end());
        OscReply r;
        REQUIRE(fx.waitForReply("/clockwork/clock/audio/input/latency/set.reply", r, 1000));
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

    EngineFixture fx{linkInputConfig()};
    fx.send(osc_test::message("/clockwork/clock/visibility",        int32_t{2}));
    fx.send(osc_test::message("/clockwork/clock/audio/publish/set", int32_t{1}));

    REQUIRE(waitForChannelVisible(fx, "FakeLive", "Main",
                                   std::chrono::seconds(30)));
    const int32_t main = subscribe(fx, "FakeLive", "Main");
    REQUIRE(main >= 0);
    REQUIRE(waitForConnected(fx, "FakeLive", "Main",
                              std::chrono::seconds(30)).state
            == kStateConnected);

    // Set lookahead to 1.5 s. Renderer ring is depth 2048 → covers
    // ~2.6 s at Link's wire rate, so 1.5 s should drop zero.
    {
        osc_test::Builder b;
        auto& s = b.begin("/clockwork/clock/audio/input/latency/set");
        s << "FakeLive" << "Main" << 1.5f;
        fx.clearReplies();
        fx.send(b.end());
    }
    OscReply setReply;
    REQUIRE(fx.waitForReply("/clockwork/clock/audio/input/latency/set.reply", setReply, 1000));
    CHECK(setReply.parsed().argInt(0) == 1);

    // Let the consumer build out the deeper retained set, then check
    // the reply field reflects the new value and that no drops occurred.
    std::this_thread::sleep_for(std::chrono::seconds(3));

    const InputSnapshot in = snapshotInput(fx, "FakeLive", "Main");
    REQUIRE(in.state >= 0);
    CHECK(std::fabs(in.latencySeconds - 1.5f) < 0.001f);
    // No drops in the steady-state with a 1.5 s window — the ring is
    // sized for it. networkGaps stays zero (loopback peer).
    CHECK(in.dropped == 0);
    CHECK(in.networkGaps == 0);
}

namespace {

constexpr double kTau = 6.283185307179586;

// What a stream sounded like, from the samples that reached the engine's input.
struct Heard {
    size_t frames = 0;   // from the first sound on
    size_t gaps   = 0;   // runs of exact silence inside the stream
    size_t jumps  = 0;   // steps between samples no sine at this pitch and level can make
    double hz     = 0.0;
    float  peak   = 0.0f;
    double pace   = 0.0;   // audio taken per second of wall clock: 1 is a device's pace
};

// Listen to one input channel for `seconds`, the engine driven as a device of
// `deviceFrames` drives it: started afresh, then a callback's worth of blocks
// at once and nothing until the next is due. What a device hears in its first
// moments, while the stream finds its place, is not judged; after that every
// sample is.
Heard listen(EngineFixture& fx, uint32_t channel, int sampleRate, uint32_t blockSize,
             uint32_t deviceFrames, double seconds, double expectHz,
             const std::function<void()>& settled) {
    constexpr double kSettleSeconds = 0.25;
    std::vector<float> stream, block;
    const size_t settle = static_cast<size_t>(kSettleSeconds * sampleRate);
    const size_t want   = settle + static_cast<size_t>(seconds * sampleRate);
    fx.restartPump();
    const auto began = std::chrono::steady_clock::now();
    for (uint64_t callback = 0; stream.size() < want; ++callback) {
        std::this_thread::sleep_until(began + std::chrono::nanoseconds(static_cast<int64_t>(
            1e9 * double(callback * deviceFrames) / double(sampleRate))));
        fx.pumpCallback(deviceFrames, [&] {
            if (snapshotBus(channel, blockSize, block))
                stream.insert(stream.end(), block.begin(), block.end());
            if (stream.size() >= settle && stream.size() < settle + blockSize) settled();
        });
    }

    Heard h;
    h.pace = (double(stream.size()) / sampleRate)
           / std::chrono::duration<double>(std::chrono::steady_clock::now() - began).count();
    size_t first = settle;
    while (first < stream.size() && stream[first] == 0.0f) ++first;
    for (size_t i = first; i < stream.size(); ++i) h.peak = std::max(h.peak, std::fabs(stream[i]));
    const float maxStep = 2.0f * h.peak * static_cast<float>(kTau * expectHz / sampleRate);
    size_t zeros = 0, rising = 0;
    for (size_t i = first + 1; i < stream.size(); ++i) {
        const float a = stream[i - 1], b = stream[i];
        if (b == 0.0f) { if (++zeros == 8) ++h.gaps; } else zeros = 0;
        if (std::fabs(b - a) > maxStep) ++h.jumps;
        if (a < 0.0f && b >= 0.0f) ++rising;
    }
    h.frames = stream.size() - first;
    if (h.frames > 0) h.hz = double(rising) * sampleRate / double(h.frames);
    return h;
}

// A Sonic Pi peer publishing its 64-frame blocks at 48 kHz, the left channel
// a 440 Hz sine, heard for two seconds by an engine at `engineRate` through a
// device of `deviceFrames`. What was heard; what the engine said of the input
// as the judging began and as it ended, read straight from it, since asking
// over OSC would pump a block out of time; and its report over OSC after.
struct Hearing {
    Heard                   heard;
    link_audio::InputStatus atStart, atEnd;
    InputSnapshot           report;
};

link_audio::InputStatus inputStatus(EngineFixture& fx, const char* peer, const char* channel) {
    for (const auto& in : fx.engine().linkAudio().listInputs())
        if (in.peerName == peer && in.channelName == channel) return in;
    return {};
}

Hearing hearSonicPiPeer(int engineRate, uint32_t deviceFrames) {
    FakeLinkPeerProcess::Options peerOpts;
    peerOpts.name       = "FakeSonicPi";
    peerOpts.blockSize  = 64;
    peerOpts.sampleRate = 48000;
    peerOpts.channels   = {{"Main", 2, "sine440-880"}};
    FakeLinkPeerProcess peer{peerOpts};
    REQUIRE(peer.ready());

    auto cfg = linkInputConfig();
    cfg.sampleRate      = engineRate;
    cfg.blockSize       = 64;
    cfg.manualAudioPump = true;
    EngineFixture fx(cfg);
    fx.send(osc_test::message("/clockwork/clock/visibility", int32_t{1}));   // this machine only
    REQUIRE(waitForChannelVisible(fx, "FakeSonicPi", "Main", std::chrono::seconds(30)));
    const int32_t pair = subscribe(fx, "FakeSonicPi", "Main");
    REQUIRE(pair >= 0);
    // Arriving, not yet healthy: until the listening starts nothing takes the
    // engine's blocks at a device's pace, and the report says so.
    REQUIRE(waitForArriving(fx, "FakeSonicPi", "Main", std::chrono::seconds(30)));
    Hearing out;
    out.heard  = listen(fx, static_cast<uint32_t>(pair), engineRate, 64, deviceFrames, 2.0, 440.0,
                        [&] { out.atStart = inputStatus(fx, "FakeSonicPi", "Main"); });
    out.atEnd  = inputStatus(fx, "FakeSonicPi", "Main");
    out.report = snapshotInput(fx, "FakeSonicPi", "Main");
    return out;
}

constexpr std::pair<int, uint32_t> kListeners[] = { {48000, 64u}, {44100, 512u} };

}  // namespace

// The report has to agree with what was heard. It said Connected, with no
// drops, over a stream nobody could listen to: the gaps were the audio thread
// reading silence, which nothing reported, and the jumps the timeline being
// lost and found again, or bent, which nothing counted. Judged over the time
// that was heard, and on the wire as well.
TEST_CASE("LinkAudio: the input report says so when what was heard was damaged",
          "[Link][LinkAudio][integration]") {
    for (const auto& [rate, frames] : kListeners) {
        const Hearing hearing = hearSonicPiPeer(rate, frames);
        const Heard& h = hearing.heard;
        const auto& a = hearing.atStart;
        const auto& b = hearing.atEnd;
        const uint64_t underruns = b.underruns - a.underruns;
        const uint64_t resyncs   = b.resyncs - a.resyncs;
        const uint64_t warps     = b.warps - a.warps;
        INFO("engine " << rate << " Hz, device buffer " << frames << ": heard " << h.gaps
             << " gaps and " << h.jumps << " jumps; meanwhile the engine counted " << underruns
             << " underruns, " << resyncs << " resyncs and " << warps << " warps, and ended in state "
             << int(b.state) << " with drift " << b.driftPpm << " ppm");
        const bool damaged = h.gaps > 0 || h.jumps > 0;
        if (h.gaps > 0)  CHECK(underruns > 0);
        if (h.jumps > 0) CHECK(resyncs + warps > 0);
        CHECK(int(b.state) == (damaged ? kStateDropout : kStateConnected));
        if (!damaged) {
            CHECK(underruns == 0);
            CHECK(resyncs == 0);
            CHECK(warps == 0);
            // Against the rates' ratio, not 1.0, which read +88,000 ppm for a
            // 48 kHz peer at 44.1 kHz. The block clock steers at up to 1000 ppm
            // while it settles after a device starts; past twice that is not
            // steering.
            CHECK(std::abs(b.driftPpm) < 2000);
        }
        // What the OSC reply carries is what the engine counted: never less.
        CHECK(hearing.report.underruns >= b.underruns);
        CHECK(hearing.report.resyncs >= b.resyncs);
        CHECK(hearing.report.warps >= b.warps);
    }
}

// What arrives has to be the peer's audio, not just audio. The engine hears
// it at its own rate, through whatever device it has: at a device's pace, but
// a whole buffer at a time. The sine has to come out whole, at pitch, with
// nothing missing.
TEST_CASE("LinkAudio: a peer's sine arrives whole at the engine's own rate and device buffer",
          "[Link][LinkAudio][integration]") {
    for (const auto& [rate, frames] : kListeners) {
        const Heard h = hearSonicPiPeer(rate, frames).heard;
        INFO("engine " << rate << " Hz, device buffer " << frames << ": " << h.frames
             << " frames heard, peak " << h.peak << ", " << h.gaps << " gaps, "
             << h.jumps << " jumps, " << h.hz << " Hz, at " << h.pace << " of a device's pace");
        CHECK(h.frames > static_cast<size_t>(rate));
        CHECK(h.peak > 0.5f);
        CHECK(h.gaps == 0);
        CHECK(h.jumps == 0);
        CHECK(std::fabs(h.hz - 440.0) < 4.4);
    }
}

#endif  // CLOCKWORK_LINK
