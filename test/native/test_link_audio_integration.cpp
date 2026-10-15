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
#include "RealtimeThread.h"
#include "ClockworkProcessor.h"
#include "OscTestUtils.h"
#include "lanes/lanes.h"
#include "shared_memory.h"  // PerformanceMetrics

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <memory>
#include <optional>
#include <atomic>
#include <sstream>
#include <thread>
#include <utility>
#include <vector>

namespace {

// Link on this machine only. Network-wide, any Link app on the network (a
// Sonic Pi or Live on another computer) joins a test's session: its tempo
// becomes the session's, and the channels are rebuilt as the sessions merge,
// so a subscription made in that moment finds nothing. The fake peer is on
// this machine only too (FakeLinkPeerProcess::Options::loopbackOnly).
constexpr int32_t kThisMachineOnly = 1;

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

// What the engine said of an input that is not as a test expected, and its
// log: whether the worker got real-time, what the stream has counted. For
// the assertion that fails next, so a red on a CI runner explains itself.
void explainInput(EngineFixture& fx, const char* what, const InputSnapshot& snap) {
    UNSCOPED_INFO(what << ": state " << snap.state << ", " << snap.bufferedMs << " ms buffered, "
                  << snap.underruns << " underruns, " << snap.resyncs << " resyncs, "
                  << snap.warps << " warps; engine log:\n" << fx.debugMessagesDump());
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
    // A refusal's reason is in the engine's log, not the reply: say it with
    // whatever assertion the -1 fails next.
    if (p.argInt(0) != 1)
        UNSCOPED_INFO("input/add " << peer << "/" << channel << " refused; engine log:\n"
                      << fx.debugMessagesDump());
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

// An engine the test pumps a few blocks at a time between its checks: far
// slower than a device, so its blocks are stamped by the wall clock, and a
// stream is heard where it is now rather than where its samples have reached.
ClockworkEngine::Config handPumpedConfig() {
    auto cfg = linkInputConfig();
    cfg.manualAudioPump = true;
    return cfg;
}

// An engine the test drives by hand, as a device would, on a freewheel clock:
// its blocks are stamped by their samples alone. A device's callbacks come on
// the hardware's time; a test's come whenever the OS wakes it — on a busy CI
// machine, tens of milliseconds late and then all at once. Stamped by when the
// callback ran, that lateness would move the timeline a peer's audio is placed
// on, and the stream would be heard to jump; stamped by its samples, the
// engine keeps a device's timeline however the test thread is scheduled.
ClockworkEngine::Config handDrivenConfig() {
    auto cfg = linkInputConfig();
    cfg.manualAudioPump = true;
    cfg.freewheelClock  = true;
    return cfg;
}

// Renders as a device does — on its own real-time thread, at a device's pace —
// until `heard` says so or `timeout` passes, and says whether it did. `heard`
// runs on that thread after each block: it is the only thread rendering, so it
// reads the buses race-free, and what it records is the test's to read once
// this returns. It must not assert: a check on that thread is not Catch's to
// catch. Pumped by hand instead — a block, then 3 ms asleep — a slow runner
// fell behind the wall clock that Link Audio is placed by (94 ms behind on
// Linux arm64), the stream never landed where the engine read, and the bus
// stayed silent.
bool renderUntil(EngineFixture& fx, const std::function<bool()>& heard, std::chrono::milliseconds timeout) {
    std::atomic<bool> done{false};
    {
        EngineFixture::Device device{fx, 48000, 128, [&] {
            if (!done.load(std::memory_order_relaxed) && heard()) done.store(true, std::memory_order_release);
        }};
        const auto deadline = std::chrono::steady_clock::now() + timeout * kTimeoutScale;
        while (!done.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return done.load(std::memory_order_acquire);
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

    EngineFixture fx{handDrivenConfig()};
    EngineFixture::Device device{fx, 48000, 512};
    // Link on (this machine only) + publish=1 so Link Audio is on and the
    // engine can see other peers' channels via link.channels().
    fx.send(osc_test::message("/clockwork/clock/visibility",         kThisMachineOnly));
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

    if (snap.state != kStateConnected) explainInput(fx, "not Connected within ten seconds", snap);
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
    fx.send(osc_test::message("/clockwork/clock/visibility",        kThisMachineOnly));
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

namespace {
// The ports open in this process, every engine's: the registry is the process's.
uint32_t openPorts() {
    return clockwork_port_list(nullptr, 0);
}
}  // namespace

// A subscription's port belongs to the process, not the engine: one nobody
// closes stays bound to its channel and holds a slot of the registry until the
// process ends. However the engine lets go of an input, its port is closed.
TEST_CASE("LinkAudio: an input the engine lets go of closes its port",
          "[Link][LinkAudio][integration]") {
    FakeLinkPeerProcess::Options peerOpts;
    peerOpts.name       = "FakeLive";
    peerOpts.channels   = {{"Main", 2, "sine440-880"}};
    FakeLinkPeerProcess peer{peerOpts};
    REQUIRE(peer.ready());

    EngineFixture fx{linkInputConfig()};
    fx.send(osc_test::message("/clockwork/clock/visibility", kThisMachineOnly));
    REQUIRE(waitForChannelVisible(fx, "FakeLive", "Main", std::chrono::seconds(30)));

    const uint32_t before = openPorts();
    REQUIRE(subscribe(fx, "FakeLive", "Main") >= 0);
    REQUIRE(openPorts() == before + 1);

    int inputsLeft = 0;
    const char* letGo = "";
    SECTION("when Link's visibility changes") {
        letGo = "Link's visibility changed";
        fx.send(osc_test::message("/clockwork/clock/visibility", int32_t{0}));   // Off
    }
    SECTION("when the clock is reset") {
        letGo = "the clock was reset";
        fx.send(osc_test::message("/clockwork/clock/reset"));
    }
    SECTION("when the inputs are cleared") {
        letGo = "the inputs were cleared";
        fx.send(osc_test::message("/clockwork/clock/audio/input/clear"));
    }
    SECTION("when the input is removed") {
        letGo = "the input was removed";
        osc_test::Builder b;
        auto& s = b.begin("/clockwork/clock/audio/input/remove");
        s << "FakeLive" << "Main";
        fx.send(b.end());
    }
    SECTION("when the same channel is subscribed again") {
        letGo = "the same channel was subscribed again";
        REQUIRE(subscribe(fx, "FakeLive", "Main") >= 0);
        inputsLeft = 1;
    }
    INFO(letGo << ": " << before << " ports were open before the subscription");
    REQUIRE(fx.pollUntil([&] { return countInputs(fx) == inputsLeft; }));
    CHECK(openPorts() == before + static_cast<uint32_t>(inputsLeft));
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
    fx.send(osc_test::message("/clockwork/clock/visibility",        kThisMachineOnly));
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
    fx.send(osc_test::message("/clockwork/clock/visibility",        kThisMachineOnly));
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
// timeout. Returns the final snapshot, explained if it is not Connected.
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
    explainInput(fx, "not Connected within the wait", snap);
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
    EngineFixture fx(handPumpedConfig());
    fx.send(osc_test::message("/clockwork/clock/visibility",        kThisMachineOnly));
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
    // Rendered at a device's pace (renderUntil), the read after each block on
    // the thread that rendered it.
    const auto* outBus = reinterpret_cast<const float*>(get_audio_output_bus());
    REQUIRE(outBus != nullptr);
    constexpr uint32_t kBlockSize = 128;
    float pL = 0.0f, pR = 0.0f, diff = 0.0f;
    renderUntil(fx, [&] {
        std::vector<float> outL(outBus,             outBus +     kBlockSize);
        std::vector<float> outR(outBus + kBlockSize, outBus + 2 * kBlockSize);
        pL   = peakAbs(outL);
        pR   = peakAbs(outR);
        diff = maxAbsDiff(outL, outR);
        return pL > 0.01f && pR > 0.01f && diff > 0.01f;
    }, std::chrono::milliseconds(5000));
    if (!(pL > 0.01f && pR > 0.01f && diff > 0.01f))
        explainInput(fx, "In.ar output silent", snapshotInput(fx, "FakeLive", "Main"));
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

    // Manual pump: the bus is read on the thread that renders it (renderUntil).
    EngineFixture fx(handPumpedConfig());
    fx.send(osc_test::message("/clockwork/clock/visibility",        kThisMachineOnly));
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
    // A bus not there yet is waited for (a check in the callback would be on
    // the device's thread, out of Catch's reach).
    renderUntil(fx, [&] {
        if (!snapshotBus(static_cast<uint32_t>(chanA), kBlockSize, busA)
            || !snapshotBus(static_cast<uint32_t>(chanB), kBlockSize, busB))
            return false;
        peakA = peakAbs(busA);
        peakB = peakAbs(busB);
        diff  = maxAbsDiff(busA, busB);
        return peakA > 0.1f && peakB > 0.3f && diff > 0.1f;
    }, std::chrono::milliseconds(5000));
    if (!(peakA > 0.1f && peakB > 0.3f && diff > 0.1f)) {
        explainInput(fx, "ChanA", snapshotInput(fx, "FakeLive", "ChanA"));
        explainInput(fx, "ChanB", snapshotInput(fx, "FakeLive", "ChanB"));
    }
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
    EngineFixture fx(handPumpedConfig());
    fx.send(osc_test::message("/clockwork/clock/visibility", kThisMachineOnly));
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
    renderUntil(fx, [&] {
        std::vector<float> outL(outBus,             outBus +     kBlockSize);
        std::vector<float> outR(outBus + kBlockSize, outBus + 2 * kBlockSize);
        pL   = peakAbs(outL);
        pR   = peakAbs(outR);
        diff = maxAbsDiff(outL, outR);
        return pL > 0.01f && pR > 0.01f && diff > 0.01f;
    }, std::chrono::milliseconds(5000));
    if (!(pL > 0.01f && pR > 0.01f && diff > 0.01f))
        explainInput(fx, "receive-only output silent", snapshotInput(fx, "FakeLive", "Main"));
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
    EngineFixture fx(handPumpedConfig());
    fx.send(osc_test::message("/clockwork/clock/visibility",        kThisMachineOnly));
    fx.send(osc_test::message("/clockwork/clock/audio/publish/set", int32_t{1}));

    REQUIRE(waitForChannelVisible(fx, "FakeLive", "Main",
                                   std::chrono::seconds(30)));
    const int32_t main = subscribe(fx, "FakeLive", "Main");
    REQUIRE(main >= 0);
    REQUIRE(waitForArriving(fx, "FakeLive", "Main", std::chrono::seconds(30)));

    // Audio on the bus before removal: rendered at a device's pace until it is
    // there (renderUntil), read on the thread that rendered it. A bus not
    // there yet is waited for.
    constexpr uint32_t kBlockSize = 128;
    std::vector<float> busL;
    const bool arrived = renderUntil(fx, [&] {
        return snapshotBus(static_cast<uint32_t>(main), kBlockSize, busL) && peakAbs(busL) > 0.01f;
    }, std::chrono::milliseconds(5000));
    if (!arrived) explainInput(fx, "no audio on the bus before removal", snapshotInput(fx, "FakeLive", "Main"));
    REQUIRE(arrived);

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

        EngineFixture fx{handDrivenConfig()};
        EngineFixture::Device device{fx, 48000, 512};
        fx.send(osc_test::message("/clockwork/clock/visibility",        kThisMachineOnly));
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
    const auto b64   = probe(120.0, "block=64",  std::chrono::seconds(2), 64);
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

    EngineFixture fx{handDrivenConfig()};
    EngineFixture::Device device{fx, 48000, 512};
    fx.send(osc_test::message("/clockwork/clock/visibility",        kThisMachineOnly));
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
    fx.send(osc_test::message("/clockwork/clock/visibility",        kThisMachineOnly));
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

    EngineFixture fx{handDrivenConfig()};
    EngineFixture::Device device{fx, 48000, 512};
    fx.send(osc_test::message("/clockwork/clock/visibility",        kThisMachineOnly));
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
    size_t silent = 0;   // of them, exact silence
    size_t gaps   = 0;   // runs of exact silence inside the stream
    size_t jumps  = 0;   // steps between samples no sine at this pitch and level can make
    size_t sinceDamage = 0;   // frames from the last gap or jump to the end
    double hz     = 0.0;
    size_t cleanFrames = 0;   // frames heard since the last gap or jump was over
    double hzClean = 0.0;     // pitch over those frames
    float  peak   = 0.0f;
    double pace   = 0.0;   // audio taken per second of wall clock: 1 is a device's pace
    double worstLateSeconds = 0.0;   // the device's latest wake after its due time
    std::string damage;   // where the gaps and jumps were, for a failure to say
    std::vector<std::pair<double, double>> gapsAt;   // each: seconds from where the judging began, length
    std::vector<double> jumpsAt;                     // seconds from where the judging began
    // The device's late wakes: when each was due, in seconds from where the
    // judging began (negative: while settling), and how late it came.
    std::vector<std::pair<double, double>> lateWakes;
    double deviceBufferSeconds = 0.0;   // one callback's worth
    // The peer's own stalls, by the same clock: when each slot it woke late
    // to publish was due and how late it came; and the slots in which it
    // could not publish at all, for a full queue.
    std::vector<std::pair<double, double>> peerLate;
    std::vector<double> peerSkipped;
};

// The engine holds a peer's audio the input latency ahead of where it reads
// (0.05 s, the renderer's default, which these cases run at). A device that
// wakes late reads the blocks it missed in a burst, which drains that margin
// by however late it was: under the latency, nothing runs dry and what is
// heard is the engine's doing; much over it, the device has made a dropout of
// its own, and what is heard says nothing about the engine. Then the case says
// it had no device and skips, rather than pass or fail on a run that never
// tested its claim. Half the latency, for margin. Shared runners wake 5-15 ms
// late at times, which is covered; a runner starved for tens of milliseconds
// is not. (A threshold of 5 ms once skipped every case on every macOS runner.)
constexpr double kInputLatencySeconds = 0.05;
constexpr double kDeviceKeptTimeSeconds = kInputLatencySeconds / 2.0;

// Whether every gap and jump heard has a stall of the machine's just before
// it, within the quarter second before the damage: the device's callback
// woke later than the dropout is long and later than half a device buffer;
// or the peer woke to publish later than the latency the engine holds, so
// the engine ran out of what it held by its own design; or the peer could
// not publish at all, so what the engine was given had the hole already.
// None of these is the engine's, and what is heard then says nothing about
// the claim under test. The explanation when every piece of damage has one,
// else empty.
std::string hostStalledBefore(const Heard& h) {
    std::ostringstream why;
    why.setf(std::ios::fixed);
    why.precision(1);
    const double floor = std::max(0.005, h.deviceBufferSeconds / 2.0);
    const auto before = [](double due, double at) { return due <= at + 0.05 && due >= at - 0.25; };
    const auto explained = [&](double at, double length, const char* what) {
        const auto say = [&](const char* who, double due, double late) {
            why << (why.tellp() > 0 ? "; " : "") << who << " due at " << due << " s woke " << late * 1e3
                << " ms late, and the " << what << " at " << at << " s followed";
        };
        for (const auto& [due, late] : h.lateWakes) {
            if (!before(due, at) || late < floor || late < length) continue;
            say("the device's callback", due, late);
            return true;
        }
        for (const auto& [due, late] : h.peerLate) {
            if (!before(due, at) || late < kInputLatencySeconds || late < length) continue;
            say("the peer's slot", due, late);
            return true;
        }
        for (const double due : h.peerSkipped) {
            if (!before(due, at)) continue;
            why << (why.tellp() > 0 ? "; " : "") << "the peer could not publish in its slot due at " << due
                << " s, and the " << what << " at " << at << " s followed";
            return true;
        }
        return false;
    };
    for (const auto& [at, length] : h.gapsAt) if (!explained(at, length, "gap")) return {};
    for (const double at : h.jumpsAt)         if (!explained(at, 0.0, "jump")) return {};
    return why.str();
}

// The rule the strict case judges by, on made-up accounts: deterministic.
TEST_CASE("LinkAudio: a gap is the machine's only when the device or the peer stalled just before it",
          "[LinkAudio]") {
    Heard h;
    h.deviceBufferSeconds = 512.0 / 44100.0;   // 11.6 ms; the floor is half of it
    h.gapsAt = {{1.71, 0.0018}};
    SECTION("a wake late by more than the gap, due just before it, explains it") {
        h.lateWakes = {{1.69, 0.02297}};
        const std::string why = hostStalledBefore(h);
        CHECK(why.find("due at 1.7 s woke 23.0 ms late") != std::string::npos);
        CHECK(why.find("gap at 1.7 s followed") != std::string::npos);
    }
    SECTION("a wake after the gap explains nothing") {
        h.lateWakes = {{1.80, 0.02297}};
        CHECK(hostStalledBefore(h).empty());
    }
    SECTION("a wake long before the gap explains nothing") {
        h.lateWakes = {{1.40, 0.02297}};
        CHECK(hostStalledBefore(h).empty());
    }
    SECTION("a wake shorter than half a device buffer explains nothing") {
        h.lateWakes = {{1.69, 0.004}};
        CHECK(hostStalledBefore(h).empty());
    }
    SECTION("a wake shorter than the gap explains nothing") {
        h.gapsAt = {{1.71, 0.030}};
        h.lateWakes = {{1.69, 0.02297}};
        CHECK(hostStalledBefore(h).empty());
    }
    SECTION("every piece of damage needs its own stall") {
        h.jumpsAt = {0.40};
        h.lateWakes = {{1.69, 0.02297}};
        CHECK(hostStalledBefore(h).empty());
        h.lateWakes.push_back({0.39, 0.012});
        CHECK(!hostStalledBefore(h).empty());
    }
    SECTION("the peer waking later than the latency the engine holds explains it") {
        h.peerLate = {{1.68, 0.061}};
        CHECK(hostStalledBefore(h).find("the peer's slot due at 1.7 s woke 61.0 ms late") != std::string::npos);
    }
    SECTION("the peer waking late, but within the latency the engine holds, explains nothing") {
        h.peerLate = {{1.68, 0.030}};
        CHECK(hostStalledBefore(h).empty());
    }
    SECTION("the peer unable to publish just before explains it") {
        h.peerSkipped = {1.70};
        CHECK(hostStalledBefore(h).find("could not publish in its slot due at 1.7 s") != std::string::npos);
    }
    SECTION("no damage needs no excuse, and gets none") {
        h.gapsAt.clear();
        h.lateWakes = {{1.69, 0.02297}};
        CHECK(hostStalledBefore(h).empty());
    }
}

void requireDeviceKeptTime(const Heard& h, int rate, uint32_t frames) {
    if (h.worstLateSeconds > kDeviceKeptTimeSeconds) {
        SKIP("this machine could not act as a " << frames << "-frame device at " << rate
             << " Hz: a callback woke " << h.worstLateSeconds * 1e3 << " ms late, so what was heard is the device's doing, not the engine's");
    }
}

// What a device hears in its first moments, while the stream finds its
// place, is not judged.
constexpr double kSettleSeconds = 0.25;

// Listen to one input channel for `seconds`, the engine driven as a device of
// `deviceFrames` drives it: started afresh, then a callback's worth of blocks
// at once and nothing until the next is due. After the settling every sample
// is judged. `progress` is told after each block how many frames have been
// heard so far, settling included, for whatever the listener wants to do at a
// moment of its choosing.
Heard analyseHeard(const std::vector<float>& stream, size_t settle, int sampleRate, double expectHz);

// A stall for the test to give the device: its callback made this much late
// once, when the judging has heard this many seconds.
struct DeviceStall { double atSeconds = 0.0; std::chrono::milliseconds by{0}; };

Heard listen(EngineFixture& fx, uint32_t channel, int sampleRate, uint32_t blockSize,
             uint32_t deviceFrames, double seconds, double expectHz,
             const std::function<void(size_t heard)>& progress, DeviceStall stall = {}) {
    std::vector<float> stream, block;
    const size_t settle = static_cast<size_t>(kSettleSeconds * sampleRate);
    const size_t want   = settle + static_cast<size_t>(seconds * sampleRate);
    // The device's thread alone touches `stream` until the device is gone;
    // this thread watches how far it has got.
    std::atomic<size_t> heard{0};
    const auto began = std::chrono::steady_clock::now();
    std::chrono::steady_clock::time_point ended;
    double worstLate = 0.0;
    std::vector<std::pair<double, double>> lateWakes;
    {
        // The device is made after its callback, which the device's thread
        // runs: so the callback reaches it through this, set once it exists.
        // The stall is asked for long after the first blocks.
        std::atomic<EngineFixture::Device*> theDevice{nullptr};
        const size_t stallAt = settle + static_cast<size_t>(stall.atSeconds * sampleRate);
        bool stalled = stall.by.count() == 0;
        EngineFixture::Device device{fx, sampleRate, deviceFrames, [&] {
            if (heard.load(std::memory_order_relaxed) >= want) return;   // enough: the rest is not kept
            if (snapshotBus(channel, blockSize, block))
                stream.insert(stream.end(), block.begin(), block.end());
            heard.store(stream.size(), std::memory_order_release);
            if (!stalled && stream.size() >= stallAt)
                if (auto* d = theDevice.load(std::memory_order_acquire)) { d->stallNextCallback(stall.by); stalled = true; }
            progress(stream.size());
        }};
        theDevice.store(&device, std::memory_order_release);
        while (heard.load(std::memory_order_acquire) < want)
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        ended = std::chrono::steady_clock::now();
        worstLate = device.worstLateSeconds();
        // The judging began when `settle` frames had been heard: at a
        // device's pace, that many seconds after the device began.
        const double judgedFrom = double(settle) / sampleRate;
        for (const auto& w : device.lateWakes())
            lateWakes.emplace_back(std::chrono::duration<double>(w.due - began).count() - judgedFrom, w.lateSeconds);
    }

    Heard h = analyseHeard(stream, settle, sampleRate, expectHz);
    h.worstLateSeconds = worstLate;
    h.lateWakes = std::move(lateWakes);
    h.deviceBufferSeconds = double(deviceFrames) / sampleRate;
    h.pace = (double(stream.size()) / sampleRate)
           / std::chrono::duration<double>(ended - began).count();
    return h;
}

// What `stream` (a bus as heard, a sine of `expectHz` once it settles) sounded
// like from `settle` on: its gaps and jumps, and its pitch.
Heard analyseHeard(const std::vector<float>& stream, size_t settle, int sampleRate, double expectHz) {
    Heard h;
    size_t first = settle;
    while (first < stream.size() && stream[first] == 0.0f) ++first;
    for (size_t i = first; i < stream.size(); ++i) h.peak = std::max(h.peak, std::fabs(stream[i]));
    const float maxStep = 2.0f * h.peak * static_cast<float>(kTau * expectHz / sampleRate);
    // Clean from where the last damage ended: the first sound after a gap,
    // the sample after a jump. (From where it began, a hole's own silence was
    // counted in the pitch after it.)
    size_t zeros = 0, rising = 0, lastDamage = first, cleanFrom = first, risingAtClean = 0;
    // Each gap and jump, in seconds from where the judging began, the first
    // dozen of them.
    std::ostringstream where;
    where.setf(std::ios::fixed);
    where.precision(3);
    size_t told = 0;
    const auto tell = [&](const char* what, size_t at, size_t frames) {
        if (frames != 0) h.gapsAt.emplace_back(double(at - settle) / sampleRate, double(frames) / sampleRate);
        else             h.jumpsAt.push_back(double(at - settle) / sampleRate);
        if (++told > 12) return;
        where << (told > 1 ? "; " : "") << what << " at " << double(at - settle) / sampleRate << " s";
        if (frames != 0) where << " for " << 1e3 * double(frames) / sampleRate << " ms";
    };
    for (size_t i = first + 1; i < stream.size(); ++i) {
        const float a = stream[i - 1], b = stream[i];
        if (b == 0.0f) {
            ++h.silent;
            if (++zeros == 8) { ++h.gaps; lastDamage = i; }
        } else {
            if (zeros >= 8) { cleanFrom = i; risingAtClean = rising; tell("gap", i - zeros, zeros); }
            zeros = 0;
        }
        if (std::fabs(b - a) > maxStep) {
            ++h.jumps; lastDamage = i; cleanFrom = i + 1; risingAtClean = rising;
            tell("jump", i, 0);
        }
        if (a < 0.0f && b >= 0.0f) ++rising;
    }
    if (zeros >= 8) tell("gap to the end", stream.size() - zeros, zeros);
    if (told > 12) where << "; and " << told - 12 << " more";
    h.damage = told == 0 ? "none" : where.str();
    h.frames = stream.size() - first;
    h.sinceDamage = stream.size() - lastDamage;
    if (h.frames > 0) h.hz = double(rising) * sampleRate / double(h.frames);
    if (zeros < 8 && cleanFrom < stream.size()) {   // not still inside a gap at the end
        h.cleanFrames = stream.size() - cleanFrom;
        h.hzClean = double(rising - risingAtClean) * sampleRate / double(h.cleanFrames);
    }
    return h;
}

// The listening tests judge a stream by its gaps, jumps and pitch. The pitch
// after the last damage is measured over the audio heard once the damage was
// over: on audio made up here, a 440 Hz sine with a 100 ms hole in it, stepped
// into and resumed smoothly, and the other way round. It was measured from
// the eighth sample of the hole, so a hole whose step came first left its own
// silence in the measure: 409 Hz, on whichever runner made the step first.
TEST_CASE("LinkAudio: what was heard is judged from where the damage ended", "[LinkAudio][analysis]") {
    const int rate = 48000;
    auto sine = [&](std::vector<float>& out, size_t frames, double phase0) {
        for (size_t i = 0; i < frames; ++i)
            out.push_back(static_cast<float>(0.8 * std::sin(phase0 + kTau * 440.0 * double(i) / rate)));
    };
    for (const bool stepIn : { true, false }) {
        DYNAMIC_SECTION((stepIn ? "stepped into, resumed smoothly" : "entered smoothly, stepped out of")) {
            std::vector<float> stream;
            // Half a second of sine; into the hole at its peak (a step) or at
            // a rising zero (smooth).
            const size_t before = static_cast<size_t>(rate / 2);
            sine(stream, before, stepIn ? kTau / 4 - kTau * 440.0 * double(before) / rate : 0.0);
            stream.insert(stream.end(), size_t(rate / 10), 0.0f);   // 100 ms of nothing
            // Out of the hole at a rising zero (smooth) or at a peak (a step).
            sine(stream, size_t(rate), stepIn ? 0.0 : kTau / 4);
            const Heard h = analyseHeard(stream, 0, rate, 440.0);
            INFO(h.gaps << " gaps, " << h.jumps << " jumps, " << h.hzClean << " Hz over the "
                 << h.cleanFrames << " frames after the damage");
            CHECK(h.gaps == 1);
            CHECK(h.jumps >= 1);
            CHECK(std::fabs(h.hzClean - 440.0) < 4.4);
            CHECK(h.cleanFrames > size_t(rate) * 9 / 10);
        }
    }
    SECTION("a stream that never came back has nothing clean after its hole") {
        std::vector<float> stream;
        sine(stream, size_t(rate / 2), 0.0);
        stream.insert(stream.end(), size_t(rate), 0.0f);
        const Heard h = analyseHeard(stream, 0, rate, 440.0);
        CHECK(h.gaps == 1);
        CHECK(h.cleanFrames == 0);
    }
}

// A Sonic Pi peer publishing its 64-frame blocks at 48 kHz, the left channel
// a 440 Hz sine, heard for two seconds by an engine at `engineRate` through a
// device of `deviceFrames`. What was heard; what the engine said of the input
// as the judging began and as it ended, read straight from it, since asking
// over OSC would pump a block out of time; and its report over OSC after.
// Beside each, the engine's own count of Link Audio underruns, as a client
// reads it from the metrics; and that count once the input was removed.
struct Hearing {
    Heard                   heard;
    link_audio::InputStatus atStart, atEnd;
    InputSnapshot           report;
    uint64_t                engineUnderrunsAtStart = 0, engineUnderrunsAtEnd = 0;
    uint64_t                engineUnderrunsAfterRemove = 0;
    std::string             engineLog;   // the engine's debug lines
    std::string             engineAccount;   // where it ran dry, and how much it held, as the judging went
    std::string             peerAccount;     // how late the peer was to publish and to send, as the judging went

    // What a failure says of it: where the damage was heard, the engine's
    // and the peer's accounts of the same time, which between them say which
    // of the two let the audio stop, and the engine's log, which says whether
    // the session held more than this test's peer.
    std::string story() const {
        return "heard: " + heard.damage + "\nthe engine: " + engineAccount + "\nthe peer: " + peerAccount
             + "\nengine log:\n" + engineLog;
    }
};

// The peer's account of the judged time, from its own timing: in each tenth
// of a second by the clock from `judgedFrom`, its latest wake to publish, the
// longest Link's thread kept it waiting to send, and the blocks it could not
// publish because that thread had not taken the ones before.
std::string peerAccountOf(const std::optional<FakeLinkPeerProcess::Timing>& t,
                          std::chrono::steady_clock::time_point judgedFrom, size_t tenths) {
    if (!t) return "its timing was not to be had";
    std::vector<int32_t> late(tenths, 0), wait(tenths, 0), skipped(tenths, 0);
    const size_t slots = std::min({t->lateUs.size(), t->waitUs.size(), t->skipped.size()});
    for (size_t i = 0; i < slots; ++i) {
        const auto at = t->t0 + t->slot * static_cast<int64_t>(i);
        if (at < judgedFrom) continue;
        const auto k = static_cast<size_t>((at - judgedFrom) / std::chrono::milliseconds(100));
        if (k >= tenths) break;
        late[k] = std::max(late[k], t->lateUs[i]);
        wait[k] = std::max(wait[k], t->waitUs[i]);
        skipped[k] += t->skipped[i];
    }
    std::ostringstream account;
    const auto row = [&](const char* what, const std::vector<int32_t>& v, bool us) {
        account << what;
        for (const int32_t x : v) account << ' ' << (us ? (x + 500) / 1000 : x);
    };
    row("in each tenth of a second by the clock, the latest it woke to publish, in ms:", late, true);
    row("\n  the longest Link's thread kept it waiting to send, in ms:", wait, true);
    row("\n  the blocks it could not publish for a full queue:", skipped, false);
    return account.str();
}

link_audio::InputStatus inputStatus(EngineFixture& fx, const char* peer, const char* channel) {
    for (const auto& in : fx.engine().linkAudio().listInputs())
        if (in.peerName == peer && in.channelName == channel) return in;
    return {};
}

uint64_t engineUnderruns(EngineFixture& fx) {
    return fx.engine().getMetrics().link_audio_underruns.load(std::memory_order_relaxed);
}

// With a `peerStall`, the peer's callback stalls for that long half a second
// into the judged time.
Hearing hearSonicPiPeer(int engineRate, uint32_t deviceFrames,
                        std::chrono::milliseconds peerStall = std::chrono::milliseconds{0},
                        DeviceStall deviceStall = {}) {
    FakeLinkPeerProcess::Options peerOpts;
    peerOpts.name       = "FakeSonicPi";
    peerOpts.blockSize  = 64;
    peerOpts.sampleRate = 48000;
    peerOpts.channels   = {{"Main", 2, "sine440-880"}};
    peerOpts.stallFor   = peerStall;
    peerOpts.reportTiming = true;
    FakeLinkPeerProcess peer{peerOpts};
    REQUIRE(peer.ready());

    auto cfg = handDrivenConfig();
    cfg.sampleRate = engineRate;
    cfg.blockSize  = 64;
    EngineFixture fx(cfg);
    fx.send(osc_test::message("/clockwork/clock/visibility", kThisMachineOnly));
    REQUIRE(waitForChannelVisible(fx, "FakeSonicPi", "Main", std::chrono::seconds(30)));
    const int32_t pair = subscribe(fx, "FakeSonicPi", "Main");
    REQUIRE(pair >= 0);
    // Arriving, not yet healthy: until the listening starts nothing takes the
    // engine's blocks at a device's pace, and the report says so.
    REQUIRE(waitForArriving(fx, "FakeSonicPi", "Main", std::chrono::seconds(30)));
    Hearing out;
    const size_t settled = static_cast<size_t>(kSettleSeconds * engineRate);
    const size_t stallAt = settled + static_cast<size_t>(engineRate) / 2;
    bool started = false, stalled = false;
    // The engine's own account as the judging goes, looked at every
    // hundredth of a second heard, in seconds from the judging's start: where
    // the input ran dry and flowed again, lost its place or bent, and in each
    // tenth of a second the least it held and the peer's buffers that arrived.
    std::ostringstream events;
    events.setf(std::ios::fixed);
    events.precision(3);
    std::vector<uint32_t> leastHeldMs, arrived;
    leastHeldMs.reserve(32);
    arrived.reserve(32);
    std::chrono::steady_clock::time_point judgedFrom;
    link_audio::InputStatus seen;
    uint64_t underrunsAtDry = 0;
    bool dry = false;
    size_t lastLook = SIZE_MAX;
    const auto event = [&](size_t at) -> std::ostream& {
        return events << (events.tellp() > 0 ? "; " : "") << double(at) / engineRate << " s ";
    };
    out.heard  = listen(fx, static_cast<uint32_t>(pair), engineRate, 64, deviceFrames, 2.0, 440.0,
                        [&](size_t heard) {
                            if (heard >= settled && !started) {
                                started = true;
                                judgedFrom = std::chrono::steady_clock::now();
                                out.atStart = inputStatus(fx, "FakeSonicPi", "Main");
                                out.engineUnderrunsAtStart = engineUnderruns(fx);
                                seen = out.atStart;
                            }
                            if (heard >= stallAt && !stalled && peerStall.count() > 0) {
                                stalled = true;
                                peer.stall();
                            }
                            if (!started) return;
                            const size_t at = heard - settled;
                            const size_t look = at * 100 / static_cast<size_t>(engineRate);
                            if (look == lastLook) return;
                            lastLook = look;
                            const auto now = inputStatus(fx, "FakeSonicPi", "Main");
                            if (leastHeldMs.size() <= look / 10) {
                                leastHeldMs.resize(look / 10 + 1, UINT32_MAX);
                                arrived.resize(look / 10 + 1, 0);
                            }
                            leastHeldMs[look / 10] = std::min(leastHeldMs[look / 10],
                                static_cast<uint32_t>(now.bufferedSeconds * 1000.0f));
                            arrived[look / 10] += static_cast<uint32_t>(
                                now.totalSourceBufferCalls - seen.totalSourceBufferCalls);
                            if (now.underruns > seen.underruns && !dry) {
                                dry = true;
                                underrunsAtDry = seen.underruns;
                                event(at) << "ran dry";
                            } else if (now.underruns == seen.underruns && dry) {
                                dry = false;
                                event(at) << "flowed again, "
                                          << 1e3 * double(now.underruns - underrunsAtDry) / engineRate << " ms missed";
                            }
                            if (now.resyncs > seen.resyncs) event(at) << "lost its place (" << now.resyncs - seen.resyncs << ")";
                            if (now.warps > seen.warps)     event(at) << "bent (" << now.warps - seen.warps << ")";
                            seen = now;
                        }, deviceStall);
    const auto listened = std::chrono::steady_clock::now();
    {
        std::ostringstream account;
        account << (events.tellp() > 0 ? events.str() : std::string("nothing happened"))
                << (dry ? ", and was dry at the end" : "")
                << "\nthe least it held in each tenth of a second, in ms:";
        for (const uint32_t ms : leastHeldMs) account << ' ' << ms;
        account << "\n  the peer's buffers that arrived in each:";
        for (const uint32_t n : arrived) account << ' ' << n;
        out.engineAccount = account.str();
    }
    const auto peerTiming = peer.timing(listened);
    out.peerAccount = peerAccountOf(peerTiming, judgedFrom, leastHeldMs.size());
    if (peerTiming) {   // the peer's stalls, in seconds from where the judging began
        const auto& t = *peerTiming;
        const size_t slots = std::min({t.lateUs.size(), t.skipped.size()});
        for (size_t i = 0; i < slots; ++i) {
            const double due = std::chrono::duration<double>(t.t0 + t.slot * static_cast<int64_t>(i) - judgedFrom).count();
            if (t.lateUs[i] > 1000) out.heard.peerLate.emplace_back(due, t.lateUs[i] / 1e6);
            if (t.skipped[i] > 0)   out.heard.peerSkipped.push_back(due);
        }
    }
    out.atEnd  = inputStatus(fx, "FakeSonicPi", "Main");
    out.engineUnderrunsAtEnd = engineUnderruns(fx);
    out.engineLog = fx.debugMessagesDump();
    out.report = snapshotInput(fx, "FakeSonicPi", "Main");

    // Removed, the input's count stays in the engine's: waited for, since the
    // metrics are published as a block begins, and given its deadline.
    {
        osc_test::Builder b;
        auto& s = b.begin("/clockwork/clock/audio/input/remove");
        s << "FakeSonicPi" << "Main";
        fx.send(b.end());
    }
    REQUIRE(fx.pollUntil([&] { return countInputs(fx) == 0; }));
    fx.pollUntil([&] { return engineUnderruns(fx) >= out.atEnd.underruns; }, 1000);
    out.engineUnderrunsAfterRemove = engineUnderruns(fx);
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
      DYNAMIC_SECTION("engine " << rate << " Hz, device buffer " << frames) {
        const Hearing hearing = hearSonicPiPeer(rate, frames);
        const Heard& h = hearing.heard;
        requireDeviceKeptTime(h, rate, frames);
        const auto& a = hearing.atStart;
        const auto& b = hearing.atEnd;
        const uint64_t underruns = b.underruns - a.underruns;
        const uint64_t resyncs   = b.resyncs - a.resyncs;
        const uint64_t warps     = b.warps - a.warps;
        INFO("engine " << rate << " Hz, device buffer " << frames << ": heard " << h.gaps
             << " gaps and " << h.jumps << " jumps; meanwhile the engine counted " << underruns
             << " underruns, " << resyncs << " resyncs and " << warps << " warps, and ended in state "
             << int(b.state) << " with drift " << b.driftPpm << " ppm");
        INFO(hearing.story());
        const bool damaged = h.gaps > 0 || h.jumps > 0;
        if (h.gaps > 0)  CHECK(underruns > 0);
        if (h.jumps > 0) CHECK(resyncs + warps > 0);
        // Dropout for a second after the last trouble, Connected again after
        // that. Damage heard around a second before the end could be either.
        const double sinceDamageSeconds = double(h.sinceDamage) / rate;
        if (!damaged || sinceDamageSeconds > 1.2)     CHECK(int(b.state) == kStateConnected);
        else if (sinceDamageSeconds < 0.8)            CHECK(int(b.state) == kStateDropout);
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
}

// What arrives has to be the peer's audio, not just audio. The engine hears
// it at its own rate, through whatever device it has: at a device's pace, but
// a whole buffer at a time. The sine has to come out whole, at pitch, with
// nothing missing.
TEST_CASE("LinkAudio: a peer's sine arrives whole at the engine's own rate and device buffer",
          "[Link][LinkAudio][integration]") {
    for (const auto& [rate, frames] : kListeners) {
      DYNAMIC_SECTION("engine " << rate << " Hz, device buffer " << frames) {
        const Hearing hearing = hearSonicPiPeer(rate, frames);
        const Heard& h = hearing.heard;
        const auto& a = hearing.atStart;
        const auto& b = hearing.atEnd;
        INFO("engine " << rate << " Hz, device buffer " << frames << ": " << h.frames
             << " frames heard, peak " << h.peak << ", " << h.gaps << " gaps, "
             << h.jumps << " jumps, " << h.hz << " Hz, at " << h.pace << " of a device's pace;"
             << " the device's worst callback " << h.worstLateSeconds * 1e3 << " ms late;"
             << " while judged the engine counted " << (b.underruns - a.underruns) << " underruns, "
             << (b.resyncs - a.resyncs) << " resyncs and " << (b.warps - a.warps) << " warps, drift "
             << b.driftPpm << " ppm");
        INFO(hearing.story());
        // A dropout the machine made, by stalling the device, is not the
        // engine's: the case did not get to test its claim, and says so.
        if (h.gaps + h.jumps > 0) {
            const std::string stalled = hostStalledBefore(h);
            if (!stalled.empty())
                SKIP("this machine stalled the " << frames << "-frame device at " << rate << " Hz: " << stalled
                     << "; what was heard is the device's doing, not the engine's");
        }
        CHECK(h.frames > static_cast<size_t>(rate));
        CHECK(h.peak > 0.5f);
        CHECK(h.gaps == 0);
        CHECK(h.jumps == 0);
        CHECK(std::fabs(h.hz - 440.0) < 4.4);
      }
    }
}

// The machine stalling the device, done on purpose: the strict case must
// then tell the device's doing from the engine's. Either the engine bridges
// the stall, and nothing is heard of it, or what is heard has the stall just
// before it, and is excused. The device's late wake is on record either way,
// placed on the judging's clock, where the gaps are.
TEST_CASE("LinkAudio: a dropout the device made is told from one the engine made",
          "[Link][LinkAudio][integration]") {
    const DeviceStall stall{1.0, std::chrono::milliseconds{80}};
    const Hearing hearing = hearSonicPiPeer(48000, 64, std::chrono::milliseconds{0}, stall);
    const Heard& h = hearing.heard;
    INFO(hearing.story());
    std::ostringstream wakes;
    for (const auto& [due, late] : h.lateWakes) wakes << due << " s +" << late * 1e3 << " ms; ";
    INFO("the device's late wakes: " << wakes.str());
    const auto theStall = std::find_if(h.lateWakes.begin(), h.lateWakes.end(), [&](const auto& w) {
        return w.second >= 0.08 && std::fabs(w.first - stall.atSeconds) < 0.1;
    });
    REQUIRE(theStall != h.lateWakes.end());
    const std::string excuse = hostStalledBefore(h);
    INFO("the excuse: " << excuse);
    CHECK(excuse.empty() == (h.gaps + h.jumps == 0));
    // And the stall is the only damage, if any: the first second was clean.
    for (const auto& [at, length] : h.gapsAt) CHECK(at > stall.atSeconds - 0.05);
    for (const double at : h.jumpsAt)         CHECK(at > stall.atSeconds - 0.05);
}


// The endpoint worker takes peers' audio off the network and resamples it
// into the ring the audio thread reads: it feeds the audio thread, from an
// ordinary thread's place in the queue, and on a busy machine it was starved
// for tens of milliseconds at a time while the ring ran dry. It is scheduled
// as the audio thread is. The engine can only ask: a Linux without rtprio for
// this user says no, which is reported, not hidden.
TEST_CASE("LinkAudio: the endpoint worker runs as a real-time thread",
          "[Link][LinkAudio][integration]") {
    FakeLinkPeerProcess::Options peerOpts;
    peerOpts.name       = "FakeLive";
    peerOpts.channels   = {{"Main", 2, "sine440-880"}};
    FakeLinkPeerProcess peer{peerOpts};
    REQUIRE(peer.ready());

    EngineFixture fx{handPumpedConfig()};
    fx.send(osc_test::message("/clockwork/clock/visibility", kThisMachineOnly));
    REQUIRE(waitForChannelVisible(fx, "FakeLive", "Main", std::chrono::seconds(30)));
    REQUIRE(subscribe(fx, "FakeLive", "Main") >= 0);

    std::optional<clockwork::RealtimeStatus> status;
    REQUIRE(fx.pollUntil([&] {
        status = fx.engine().linkAudio().endpointRealtime();
        return status.has_value();
    }, 5000));
    INFO("the OS answered " << int(*status));
#if defined(__linux__)
    CHECK((*status == clockwork::RealtimeStatus::Applied
           || *status == clockwork::RealtimeStatus::NotPermitted));
#else
    CHECK(*status == clockwork::RealtimeStatus::Applied);
#endif
}

// The engine's metric for the Link Audio it holds is its input's own figure.
// It counted the renderer's queue alone, which the endpoint worker empties
// into the port ring as fast as there is room: a stream playing cleanly,
// holding its latency's worth, read 0 ms.
TEST_CASE("LinkAudio: the buffered metric counts all an input holds",
          "[Link][LinkAudio][integration]") {
    FakeLinkPeerProcess::Options peerOpts;
    peerOpts.name     = "FakeLive";
    peerOpts.channels = {{"Main", 2, "sine440-880"}};
    FakeLinkPeerProcess peer{peerOpts};
    REQUIRE(peer.ready());

    // Both read on the device's thread after each block for a second: the
    // metric as published when the block began, the input's figure as it
    // ended. A block apart, and the metric in whole milliseconds, so within
    // a block and a millisecond of each other.
    constexpr int kLooks = 750;
    constexpr double kApartMs = 1e3 * 64.0 / 48000.0 + 1.0;
    std::atomic<bool> looking{false};
    std::atomic<int> looks{0};
    double metricMs = 0.0, inputMs = 0.0;
    auto cfg = handDrivenConfig();
    cfg.blockSize = 64;
    EngineFixture fx{cfg};
    EngineFixture::Device device{fx, 48000, 64, [&] {
        if (!looking.load(std::memory_order_acquire) || looks.load(std::memory_order_relaxed) == kLooks) return;
        metricMs += fx.engine().getMetrics().link_audio_buffered_ms.load(std::memory_order_relaxed);
        inputMs  += 1e3 * inputStatus(fx, "FakeLive", "Main").bufferedSeconds;
        looks.fetch_add(1, std::memory_order_release);
    }};
    fx.send(osc_test::message("/clockwork/clock/visibility", kThisMachineOnly));
    REQUIRE(waitForChannelVisible(fx, "FakeLive", "Main", std::chrono::seconds(30)));
    REQUIRE(subscribe(fx, "FakeLive", "Main") >= 0);
    REQUIRE(waitForConnected(fx, "FakeLive", "Main", std::chrono::seconds(10)).state == kStateConnected);

    looking.store(true, std::memory_order_release);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (looks.load(std::memory_order_acquire) < kLooks && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    REQUIRE(looks.load(std::memory_order_acquire) == kLooks);

    INFO("over " << kLooks << " blocks the metric said " << metricMs / kLooks << " ms on average, and the input "
         << inputMs / kLooks << " ms");
    CHECK(inputMs / kLooks > 5.0);
    CHECK(std::fabs(metricMs - inputMs) / kLooks < kApartMs);
}

// A peer whose callback stalls makes no audio for that long, and the engine
// hears it: a gap at least that long, the place lost and found again (a
// resync, never a bend), and then the peer's stream carrying on at its own
// pitch — not the missed audio played fast to catch up, not running late —
// with the report Connected again once it is over.
//
// The exact account — one gap, its length, one resync, no chirp at its edges —
// is the renderer's, and Clockwork pins it on made-up time, where it is
// deterministic ("a hole in the peer's stream is nothing to render, then the
// stream resumes"; "after the host has gone on without it, the stream rejoins
// the present"). Here two processes run on real clocks, and a shared runner
// that starves a thread for a moment adds a dropout of its own; held to the
// exact count, this failed on whichever runner did that day.
TEST_CASE("LinkAudio: a peer that stalls is heard to drop out and then carry on at pitch",
          "[Link][LinkAudio][integration]") {
    for (const auto& [rate, frames] : kListeners) {
      DYNAMIC_SECTION("engine " << rate << " Hz, device buffer " << frames) {
        const Hearing hearing = hearSonicPiPeer(rate, frames, std::chrono::milliseconds{100});
        const Heard& h = hearing.heard;
        requireDeviceKeptTime(h, rate, frames);
        const auto& a = hearing.atStart;
        const auto& b = hearing.atEnd;
        INFO("engine " << rate << " Hz, device buffer " << frames << ": " << h.frames
             << " frames heard, " << h.gaps << " gaps, " << h.jumps << " jumps; "
             << h.hzClean << " Hz over the " << h.cleanFrames << " frames after the last damage;"
             << " the engine counted " << (b.underruns - a.underruns) << " underruns, "
             << (b.resyncs - a.resyncs) << " resyncs and " << (b.warps - a.warps) << " warps; "
             << "the peer stalled 0.5 s in");
        INFO(hearing.story());
        // The stall is heard: silence at least as long as it.
        CHECK(h.gaps >= 1);
        CHECK(h.silent > size_t(rate) / 20);
        CHECK(b.underruns > a.underruns);
        // Lost and found again, never bent to catch up.
        CHECK(b.resyncs > a.resyncs);
        CHECK(b.warps == a.warps);
        // And on from there at the peer's pitch, given a quarter second to
        // measure it over.
        // A stream that never came back has no clean frames after the hole,
        // and fails here.
        CHECK(h.cleanFrames > size_t(rate) / 4);
        if (h.cleanFrames > size_t(rate) / 4)
            CHECK(std::fabs(h.hzClean - 440.0) < 4.4);
        // Connected again over a second after the last trouble.
        if (double(h.sinceDamage) / rate > 1.2)
            CHECK(int(b.state) == kStateConnected);
        // The engine's count, the one a client reads from the metrics, is its
        // inputs': it rose with this one's, never past it, and kept it once
        // the input was removed.
        INFO("the engine's metric counted " << hearing.engineUnderrunsAtStart << " underruns at the start, "
             << hearing.engineUnderrunsAtEnd << " at the end and " << hearing.engineUnderrunsAfterRemove
             << " after the input was removed; the input counted " << a.underruns << " and " << b.underruns);
        CHECK(hearing.engineUnderrunsAtEnd > hearing.engineUnderrunsAtStart);
        CHECK(hearing.engineUnderrunsAtEnd <= b.underruns);
        CHECK(hearing.engineUnderrunsAfterRemove >= b.underruns);
      }
    }
}

#endif  // CLOCKWORK_LINK
