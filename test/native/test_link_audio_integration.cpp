// test_link_audio_integration.cpp — SuperSonic's Link Audio inputs.
//
// Most cases hear a stand-in peer (LinkAudioHost::addStandInChannel, below).
// Its channels are listed and subscribed over OSC as a Link peer's are, and
// its audio reaches the engine as a Link peer's does, cut into Link's wire
// chunks, but sent by the case. Link stays off, and the test pumps every
// block, runs the control pass and runs the endpoint, so these cases come out
// the same on every run. Clockwork's test_link_audio_stand_in.cpp judges what
// a peer's audio sounds like on its way in; these check what SuperSonic does
// with it: the OSC replies, the lanes an input gets, scsynth hearing it, and
// the engine's own counts.
//
// The last case uses real Link: a peer in another process, found over the
// network. It shows the real path works, and it cannot be exact; when it
// fails, its waits say what the engine and the peer each saw.
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
#include "OscTestUtils.h"
#include "lanes/lanes.h"
#include "shared_memory.h"  // PerformanceMetrics

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <functional>
#include <optional>
#include <atomic>
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
constexpr int kStateConnected = 2;
constexpr int kStateDropout   = 3;

// How many Link peers the engine counts, or -1 when it does not answer.
int enginePeers(EngineFixture& fx) {
    fx.clearReplies();
    fx.send(osc_test::message("/clockwork/clock/peers/count/get"));
    OscReply reply;
    if (!fx.waitForReply("/clockwork/clock/peers/count.reply", reply, 500)) return -1;
    return reply.parsed().argInt(0);
}

// Poll /clock/audio/channels/get until the engine lists `channelName` from
// `peer`, or the wait ends. Then it says what each side saw: the channels the
// engine listed and the Link peers it counts, and the peer process with the
// peers and channels it saw, so a discovery that never finished names the
// side it failed on.
bool waitForChannelVisible(EngineFixture& fx,
                           const FakeLinkPeerProcess& peer,
                           const std::string& channelName,
                           std::chrono::milliseconds timeout) {
    using clock = std::chrono::steady_clock;
    const std::string& peerName = peer.options().name;
    timeout *= kTimeoutScale;  // sanitizer builds run slower; scale the wait
    const auto deadline = clock::now() + timeout;
    int asked = 0, answered = 0;
    std::string listed = "nothing";
    while (clock::now() < deadline) {
        fx.clearReplies();
        fx.send(osc_test::message("/clockwork/clock/audio/channels/get"));
        ++asked;
        OscReply reply;
        if (fx.waitForReply("/clockwork/clock/audio/channels.reply", reply, 200)) {
            ++answered;
            const auto p = reply.parsed();
            const int count = p.argInt(0);
            listed = count == 0 ? "no channels" : "";
            // Per entry: [channelId:s channelName:s peerId:s peerName:s]
            for (int i = 0; i < count; ++i) {
                const int base = 1 + i * 4;
                if (p.argString(base + 1) == channelName &&
                    p.argString(base + 3) == peerName) {
                    return true;
                }
                listed += (i ? ", " : "") + p.argString(base + 3) + "/" + p.argString(base + 1);
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    UNSCOPED_INFO("The engine never listed " << peerName << "/" << channelName << " in "
                  << timeout.count() / 1000.0 << " s. It answered " << answered << " of " << asked
                  << " channel requests, the last listing " << listed << ", and counts "
                  << enginePeers(fx) << " Link peers. " << peer.account()
                  << "\nEngine log:\n" << fx.debugMessagesDump());
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
float inBusFor(ClockworkCore* core, int32_t channel) {
    return static_cast<float>(static_cast<int>(clockwork_out_channels(core)) + channel);
}

// Six input channels, as a device might have. A stream never lands on them:
// the engine puts it in its Link Audio lanes, above every device channel.
//
// Link runs threads of its own and hears its peers over the network: what a
// case here waits for needs real time, however fast the test renders. So the
// engine's own gateway delivers, and the fixture's waits keep the wall clock
// (EngineFixture::testIsTheHost).
ClockworkEngine::Config linkInputConfig() {
    auto cfg = EngineFixture::defaultConfig();
    cfg.numInputChannels  = 6;
    cfg.hostDrivesControl = false;
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

namespace {

// The ports open in the engine's core: each core's are its own.
uint32_t openPorts(EngineFixture& fx) {
    return clockwork_port_list(clockwork_core_ports(fx.engine().core()), nullptr, 0);
}

// Read `blockSize` samples from one INPUT CHANNEL into `out`.
//
// It used to read the DSP's private bus pool through get_audio_bus_pool(), an
// accessor the seam narrowing removed — clockwork does not hand out the
// guest's signal storage. Peer audio arrives as ordinary input channels now
// (a source port bound through clockwork_port_bus.h), so this reads the input bus
// clockwork does expose. No snapshot, so a torn-mid-write read is possible;
// for amplitude and L!=R assertions the race does not change the outcome.
bool snapshotBus(ClockworkCore* core, uint32_t chIdx, uint32_t blockSize, std::vector<float>& out) {
    const auto* in = clockwork_audio_in(core);
    if (!in) return false;
    if (chIdx >= static_cast<uint32_t>(static_cast<int>(clockwork_in_channels(core)))) return false;
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

// ── A stand-in peer ────────────────────────────────────────────────────────

constexpr double kTau = 6.283185307179586;
constexpr const char* kStandIn = "FakeLive";   // the stand-in peer's name

// SuperSonic's pumped engine, with six device inputs below the Link Audio
// lanes, as a device might have.
ClockworkEngine::Config standInConfig() {
    auto cfg = EngineFixture::defaultConfig();
    cfg.numInputChannels = 6;
    return cfg;
}

// What one of the stand-in's channels plays: `channels` (1 or 2) of
// `signal(frame, channel)` at 48 kHz.
struct PeerChannel {
    std::string name;
    uint32_t    channels;
    std::function<double(uint64_t, uint32_t)> signal;
};

// 440 Hz on the left and 880 Hz on the right, so L and R are told apart end
// to end.
double sine440and880(uint64_t f, uint32_t channel) {
    return 0.5 * std::sin(kTau * (channel == 0 ? 440.0 : 880.0) * double(f) / 48000.0);
}

std::function<double(uint64_t, uint32_t)> steady(double v) {
    return [v](uint64_t, uint32_t) { return v; };
}

// The stand-in peer. It plays from the moment it is made, `block` frames a
// buffer stamped from its own frame count, as a device stamps a callback.
// After every block the fixture renders (setHostWork) it sends what it has
// made by the time the next block is read, and the endpoint runs: a stream
// that never waits on the machine, through a case's own pumping and through
// every wait for a reply.
class StandInPeer {
public:
    StandInPeer(EngineFixture& fx, std::vector<PeerChannel> channels, uint32_t block = 64,
                bool endpointByHand = true)
        : mFx(fx), mLink(fx.engine().linkAudio()), mChannels(std::move(channels)), mBlock(block) {
        mLink.setEndpointByHand(endpointByHand);
        for (const auto& c : mChannels) mLink.addStandInChannel(kStandIn, c.name.c_str());
        mFx.setHostWork([this] { turn(); });
    }
    ~StandInPeer() { mFx.setHostWork({}); }
    StandInPeer(const StandInPeer&) = delete;
    StandInPeer& operator=(const StandInPeer&) = delete;

    // The peer's callback stops for `seconds`: what it would have made in
    // that time is never made, and it goes on stamped at the present.
    void stall(double seconds) {
        mFrame += static_cast<uint64_t>(seconds * kRate) / mBlock * mBlock;
    }

private:
    static constexpr uint32_t kRate = 48000;

    uint64_t microsOf(uint64_t f) const {
        return mT0 + static_cast<uint64_t>(std::llround(1e6 * double(f) / kRate));
    }

    void turn() {
        const uint64_t now = mLink.nextReadMicros();
        if (now == 0) return;
        if (mT0 == 0) mT0 = now;
        while (microsOf(mFrame) <= now) {
            for (const auto& c : mChannels) {
                mSamples.resize(size_t(mBlock) * c.channels);
                for (uint32_t i = 0; i < mBlock; ++i)
                    for (uint32_t ch = 0; ch < c.channels; ++ch)
                        mSamples[size_t(i) * c.channels + ch] = static_cast<float>(c.signal(mFrame + i, ch));
                mLink.sendStandInAudio(kStandIn, c.name.c_str(), mSamples.data(), mBlock, c.channels,
                                       kRate, microsOf(mFrame));
            }
            mFrame += mBlock;
        }
        mLink.runEndpoint();
    }

    EngineFixture&           mFx;
    LinkAudioHost&           mLink;
    std::vector<PeerChannel> mChannels;
    const uint32_t           mBlock;
    uint64_t                 mT0 = 0;
    uint64_t                 mFrame = 0;
    std::vector<float>       mSamples;
};

// The stand-in hears what the test pumps: under SUPERSONIC_TEST_REALTIME the
// engine renders on a thread of its own, and nothing would run it.
#define REQUIRE_TEST_IS_THE_HOST(fx)                                                      \
    do {                                                                                  \
        if (!(fx).testIsTheHost()) SKIP("the stand-in peer needs the test to pump every block"); \
    } while (0)

// Blocks of audio in `seconds`, at the engine's rate and block size.
uint32_t blocksIn(EngineFixture& fx, double seconds) {
    return static_cast<uint32_t>(std::ceil(seconds * 48000.0 / clockwork_block_size(fx.engine().core())));
}

std::vector<float> inputChannel(EngineFixture& fx, int32_t channel) {
    std::vector<float> out;
    REQUIRE(snapshotBus(fx.engine().core(), static_cast<uint32_t>(channel),
                        clockwork_block_size(fx.engine().core()), out));
    return out;
}

void removeInput(EngineFixture& fx, const char* channel) {
    osc_test::Builder b;
    b.begin("/clockwork/clock/audio/input/remove") << kStandIn << channel;
    fx.send(b.end());
}

int setLatency(EngineFixture& fx, const char* channel, float seconds) {
    osc_test::Builder b;
    b.begin("/clockwork/clock/audio/input/latency/set") << kStandIn << channel << seconds;
    fx.clearReplies();
    fx.send(b.end());
    OscReply r;
    REQUIRE(fx.waitForReply("/clockwork/clock/audio/input/latency/set.reply", r, 1000));
    return r.parsed().argInt(0);
}

link_audio::InputStatus inputStatus(EngineFixture& fx, const char* channel) {
    for (const auto& in : fx.engine().linkAudio().listInputs())
        if (in.peerName == kStandIn && in.channelName == channel) return in;
    return {};
}

}  // namespace

// A reset takes Link down and up again, which is what ends the inputs: an
// input exists only while Link is on, since its channel came over Link. So
// Link is on here, on this machine only, though the stand-in needs none of it.
TEST_CASE("LinkAudio: /clockwork/clock/reset removes every input", "[LinkAudio][stand-in]") {
    EngineFixture fx{standInConfig()};
    REQUIRE_TEST_IS_THE_HOST(fx);
    StandInPeer peer{fx, {{"Main", 2, sine440and880}}};
    fx.send(osc_test::message("/clockwork/clock/visibility", kThisMachineOnly));
    REQUIRE(subscribe(fx, kStandIn, "Main") >= 0);
    REQUIRE(countInputs(fx) == 1);

    fx.send(osc_test::message("/clockwork/clock/reset"));
    CHECK(fx.pollUntil([&] { return countInputs(fx) == 0; }));
}

// A subscription's port belongs to the process, not the engine: one nobody
// closes stays bound to its channel and holds a slot of the registry until the
// process ends. Link is on, as it is whenever an input exists, so that
// visibility and a reset have Link to take down.
TEST_CASE("LinkAudio: an input's port is closed however the input ends", "[LinkAudio][stand-in]") {
    EngineFixture fx{standInConfig()};
    REQUIRE_TEST_IS_THE_HOST(fx);
    StandInPeer peer{fx, {{"Main", 2, sine440and880}}};
    fx.send(osc_test::message("/clockwork/clock/visibility", kThisMachineOnly));

    const uint32_t before = openPorts(fx);
    REQUIRE(subscribe(fx, kStandIn, "Main") >= 0);
    REQUIRE(openPorts(fx) == before + 1);

    int inputsLeft = 0;
    const char* letGo = "";
    SECTION("when Link's visibility goes Off") {
        letGo = "Link's visibility went Off";
        fx.send(osc_test::message("/clockwork/clock/visibility", int32_t{0}));
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
        removeInput(fx, "Main");
    }
    SECTION("when the same channel is subscribed again") {
        letGo = "the same channel was subscribed again";
        REQUIRE(subscribe(fx, kStandIn, "Main") >= 0);
        inputsLeft = 1;
    }
    INFO(letGo << ": " << before << " ports were open before the subscription");
    REQUIRE(fx.pollUntil([&] { return countInputs(fx) == inputsLeft; }));
    CHECK(openPorts(fx) == before + static_cast<uint32_t>(inputsLeft));
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

// `count` mono channels, C0, C1 ... so a case can ask for more subscriptions
// than the lanes hold.
std::vector<PeerChannel> monoChannels(int count) {
    std::vector<PeerChannel> out;
    for (int i = 0; i < count; ++i) out.push_back({"C" + std::to_string(i), 1, steady(0.25)});
    return out;
}

}  // namespace

TEST_CASE("LinkAudio: each subscription gets a pair of the Link Audio lanes, and the same stream keeps its pair",
          "[LinkAudio][stand-in]") {
    EngineFixture fx{standInConfig()};
    REQUIRE_TEST_IS_THE_HOST(fx);
    StandInPeer peer{fx, monoChannels(2)};

    const auto base = static_cast<int32_t>(clockwork_link_audio_lane_base(fx.engine().core()));
    REQUIRE(base >= static_cast<int32_t>(standInConfig().numInputChannels));
    CHECK(subscribe(fx, kStandIn, "C0") == base);
    CHECK(subscribe(fx, kStandIn, "C1") == base + 2);
    CHECK(subscribe(fx, kStandIn, "C0") == base);
}

TEST_CASE("LinkAudio: a removed subscription's pair goes to the next one, and the others stay put",
          "[LinkAudio][stand-in]") {
    EngineFixture fx{standInConfig()};
    REQUIRE_TEST_IS_THE_HOST(fx);
    StandInPeer peer{fx, monoChannels(3)};

    const int32_t c0 = subscribe(fx, kStandIn, "C0");
    const int32_t c1 = subscribe(fx, kStandIn, "C1");
    REQUIRE(c0 >= 0);
    REQUIRE(c1 >= 0);
    removeInput(fx, "C0");
    REQUIRE(fx.pollUntil([&] { return countInputs(fx) == 1; }));

    CHECK(subscribe(fx, kStandIn, "C2") == c0);
    CHECK(subscribe(fx, kStandIn, "C1") == c1);
}

// Refused rather than accepted: a stream bound past the lanes would be skipped
// by pull_port_sources and look exactly like a working subscription that is
// silent for ever.
TEST_CASE("LinkAudio: a subscription the lanes have no room for is refused", "[LinkAudio][stand-in]") {
    const auto pairs = static_cast<int>(LinkAudioBridge::kLanes / 2);
    EngineFixture fx{standInConfig()};
    REQUIRE_TEST_IS_THE_HOST(fx);
    StandInPeer peer{fx, monoChannels(pairs + 1)};
    REQUIRE(clockwork_link_audio_lanes(fx.engine().core()) == LinkAudioBridge::kLanes);

    for (int i = 0; i < pairs; ++i)
        REQUIRE(subscribe(fx, kStandIn, ("C" + std::to_string(i)).c_str()) >= 0);
    CHECK(subscribe(fx, kStandIn, ("C" + std::to_string(pairs)).c_str()) == -1);
}

// A request's trailing int32 comes back as its reply's last argument, as on
// every other /clockwork/clock reply, so a client matching replies by token
// (Sonic Pi's rpc) can read these at all.
TEST_CASE("LinkAudio: input and channel replies end with the request's token", "[LinkAudio][stand-in]") {
    EngineFixture fx{standInConfig()};
    REQUIRE_TEST_IS_THE_HOST(fx);
    StandInPeer peer{fx, monoChannels(1)};

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
        b.begin("/clockwork/clock/audio/input/add") << kStandIn << "C0" << int32_t{4242};
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

// The synth reads the scsynth bus the input's channel is (inBusFor), and
// stereo_passthrough writes both channels unchanged to output 0 and 1: what
// comes out is what arrived, L and R kept apart, sample for sample.
TEST_CASE("LinkAudio: a synth's In.ar hears an input's two channels exactly as they arrived",
          "[LinkAudio][stand-in]") {
    EngineFixture fx{standInConfig()};
    REQUIRE_TEST_IS_THE_HOST(fx);
    StandInPeer peer{fx, {{"Main", 2, sine440and880}}};
    const int32_t main = subscribe(fx, kStandIn, "Main");
    REQUIRE(main >= 0);
    REQUIRE(fx.loadSynthDef("stereo_passthrough"));
    {
        osc_test::Builder b;
        b.begin("/s_new") << "stereo_passthrough" << int32_t{1000} << int32_t{0} << int32_t{1}
                          << "in_bus" << inBusFor(fx.engine().core(), main) << "out" << 0.0f;
        fx.send(b.end());
        OscReply r;
        fx.send(osc_test::message("/sync", 42));
        REQUIRE(fx.waitForReply("/synced", r));
    }
    fx.pumpBlock(blocksIn(fx, 0.25));   // the input's latency, and some

    ClockworkCore* core = fx.engine().core();
    const uint32_t n = clockwork_block_size(core);
    float peakL = 0, peakR = 0, apart = 0, worst = 0;
    for (uint32_t k = 0; k < blocksIn(fx, 0.25); ++k) {
        fx.pumpBlock();
        const auto inL = inputChannel(fx, main), inR = inputChannel(fx, main + 1);
        const float* out = clockwork_audio_out(core);
        REQUIRE(out != nullptr);
        const std::vector<float> outL(out, out + n), outR(out + n, out + 2 * n);
        peakL = std::max(peakL, peakAbs(outL));
        peakR = std::max(peakR, peakAbs(outR));
        apart = std::max(apart, maxAbsDiff(outL, outR));
        worst = std::max({worst, maxAbsDiff(outL, inL), maxAbsDiff(outR, inR)});
    }
    INFO("output peaks " << peakL << " and " << peakR << ", L and R up to " << apart
         << " apart, and up to " << worst << " from the input");
    CHECK(peakL > 0.4f);
    CHECK(peakR > 0.4f);
    CHECK(apart > 0.1f);
    CHECK(worst == 0.0f);
}

TEST_CASE("LinkAudio: two inputs arrive on two pairs, each with only its own audio", "[LinkAudio][stand-in]") {
    EngineFixture fx{standInConfig()};
    REQUIRE_TEST_IS_THE_HOST(fx);
    // Mono, so each arrives on both channels of its pair.
    StandInPeer peer{fx, {{"Sine", 1, [](uint64_t f, uint32_t) { return sine440and880(f, 0); }},
                          {"Level", 1, steady(0.5)}}};
    const int32_t sine = subscribe(fx, kStandIn, "Sine");
    const int32_t level = subscribe(fx, kStandIn, "Level");
    REQUIRE(sine >= 0);
    REQUIRE(level >= 0);
    REQUIRE(sine != level);
    fx.pumpBlock(blocksIn(fx, 0.25));

    float sinePeak = 0, sineMirror = 0, levelOff = 0, levelMirror = 0;
    for (uint32_t k = 0; k < blocksIn(fx, 0.25); ++k) {
        fx.pumpBlock();
        const auto sL = inputChannel(fx, sine), sR = inputChannel(fx, sine + 1);
        const auto lL = inputChannel(fx, level), lR = inputChannel(fx, level + 1);
        sinePeak    = std::max(sinePeak, peakAbs(sL));
        sineMirror  = std::max(sineMirror, maxAbsDiff(sL, sR));
        levelMirror = std::max(levelMirror, maxAbsDiff(lL, lR));
        for (const float v : lL) levelOff = std::max(levelOff, std::fabs(v - 0.5f));
    }
    INFO("the sine peaked at " << sinePeak << "; the level was up to " << levelOff << " from 0.5");
    CHECK(sinePeak > 0.4f);
    CHECK(sineMirror == 0.0f);
    CHECK(levelOff < 1e-3f);
    CHECK(levelMirror == 0.0f);
}

TEST_CASE("LinkAudio: a removed input stops writing its pair", "[LinkAudio][stand-in]") {
    EngineFixture fx{standInConfig()};
    REQUIRE_TEST_IS_THE_HOST(fx);
    StandInPeer peer{fx, {{"Main", 2, sine440and880}}};
    const int32_t main = subscribe(fx, kStandIn, "Main");
    REQUIRE(main >= 0);
    fx.pumpBlock(blocksIn(fx, 0.25));
    REQUIRE(peakAbs(inputChannel(fx, main)) > 0.1f);

    removeInput(fx, "Main");
    REQUIRE(fx.pollUntil([&] { return countInputs(fx) == 0; }));
    fx.pumpBlock(4);
    const auto first = inputChannel(fx, main);
    fx.pumpBlock(4);
    const auto later = inputChannel(fx, main);
    INFO("the pair moved by up to " << maxAbsDiff(first, later) << " after the input went");
    CHECK(maxAbsDiff(first, later) == 0.0f);
}

// Across Link's tempo range (20 to 999 bpm) and the buffer sizes peers
// publish, at the default 50 ms latency: nothing is dropped, nothing is
// missed, and the input reads Connected.
TEST_CASE("LinkAudio: nothing is dropped at the default latency, at any tempo or peer buffer size",
          "[LinkAudio][stand-in]") {
    struct Point { double bpm; uint32_t peerBlock; };
    for (const auto& [bpm, peerBlock] : {Point{20, 1024}, Point{120, 1024}, Point{999, 1024},
                                         Point{120, 64}, Point{120, 256}, Point{120, 2048}}) {
        DYNAMIC_SECTION(bpm << " bpm, peer buffer " << peerBlock) {
            EngineFixture fx{standInConfig()};
            REQUIRE_TEST_IS_THE_HOST(fx);
            fx.send(osc_test::message("/clockwork/clock/tempo/set", static_cast<float>(bpm)));
            StandInPeer peer{fx, {{"Main", 2, sine440and880}}, peerBlock};
            REQUIRE(subscribe(fx, kStandIn, "Main") >= 0);
            fx.pumpBlock(blocksIn(fx, 2.0));
            const InputSnapshot in = snapshotInput(fx, kStandIn, "Main");
            INFO(in.sourceCalls << " buffers arrived; " << in.dropped << " dropped, " << in.networkGaps
                 << " missing, " << in.underruns << " frames heard short, state " << in.state);
            CHECK(in.sourceCalls > 0);
            CHECK(in.dropped == 0);
            CHECK(in.networkGaps == 0);
            CHECK(in.underruns == 0);
            CHECK(in.state == kStateConnected);
        }
    }
}

// The same stream again is the same input: its pair, and its renderer, so its
// counts carry on rather than start again.
TEST_CASE("LinkAudio: subscribing to the same channel again keeps its pair and its counts",
          "[LinkAudio][stand-in]") {
    EngineFixture fx{standInConfig()};
    REQUIRE_TEST_IS_THE_HOST(fx);
    StandInPeer peer{fx, {{"Main", 2, sine440and880}}};
    const int32_t main = subscribe(fx, kStandIn, "Main");
    REQUIRE(main >= 0);
    fx.pumpBlock(blocksIn(fx, 0.5));
    const uint64_t before = snapshotInput(fx, kStandIn, "Main").sourceCalls;
    REQUIRE(before > 0);

    REQUIRE(subscribe(fx, kStandIn, "Main") == main);
    const uint64_t after = snapshotInput(fx, kStandIn, "Main").sourceCalls;
    INFO(before << " buffers before, " << after << " after");
    CHECK(after >= before);
}

// Live's UI stops at 2 s, and the renderer's queue holds that much: more is
// refused, so a caller is told rather than hearing it as dropouts.
TEST_CASE("LinkAudio: the latency setter refuses more than the 2 s an input can hold",
          "[LinkAudio][stand-in]") {
    EngineFixture fx{standInConfig()};
    REQUIRE_TEST_IS_THE_HOST(fx);
    StandInPeer peer{fx, {{"Main", 2, sine440and880}}};
    REQUIRE(subscribe(fx, kStandIn, "Main") >= 0);

    CHECK(setLatency(fx, "Main", 0.05f) == 1);
    CHECK(setLatency(fx, "Main", 2.0f) == 1);
    CHECK(setLatency(fx, "Main", 2.5f) == 0);
    CHECK(setLatency(fx, "Main", 10.0f) == 0);
    CHECK(setLatency(fx, "Main", 1e9f) == 0);
}

TEST_CASE("LinkAudio: an input's latency is reported back, and 1.5 s of it drops nothing",
          "[LinkAudio][stand-in]") {
    EngineFixture fx{standInConfig()};
    REQUIRE_TEST_IS_THE_HOST(fx);
    StandInPeer peer{fx, {{"Main", 2, sine440and880}}};
    REQUIRE(subscribe(fx, kStandIn, "Main") >= 0);
    REQUIRE(setLatency(fx, "Main", 1.5f) == 1);
    fx.pumpBlock(blocksIn(fx, 3.0));

    const InputSnapshot in = snapshotInput(fx, kStandIn, "Main");
    INFO("latency " << in.latencySeconds << " s; " << in.dropped << " dropped, " << in.networkGaps
         << " missing, " << in.underruns << " frames heard short, state " << in.state);
    CHECK(std::fabs(in.latencySeconds - 1.5f) < 0.001f);
    CHECK(in.dropped == 0);
    CHECK(in.networkGaps == 0);
    CHECK(in.underruns == 0);
    CHECK(in.state == kStateConnected);
}

// The endpoint worker resamples a peer's audio into the ring the audio thread
// reads, so it is scheduled as the audio thread is: from an ordinary thread's
// place in the queue it was starved for tens of milliseconds at a time on a
// busy machine. The engine can only ask; a Linux without rtprio for this user
// says no, which is reported, not hidden. Its own thread here, not run by
// hand.
TEST_CASE("LinkAudio: the endpoint worker asks to run as a real-time thread", "[LinkAudio][stand-in]") {
    EngineFixture fx{standInConfig()};
    REQUIRE_TEST_IS_THE_HOST(fx);
    StandInPeer peer{fx, {{"Main", 2, sine440and880}}, 64, /*endpointByHand=*/false};
    REQUIRE(subscribe(fx, kStandIn, "Main") >= 0);

    std::optional<clockwork::RealtimeStatus> status;
    REQUIRE(fx.waitForThread([&] {
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

// The engine's metric for the Link Audio it holds is its input's own figure:
// what the renderer holds and what the port holds. It counted the renderer's
// queue alone, which the endpoint empties into the port as fast as there is
// room, so a stream playing cleanly read 0 ms. The metric is published as a
// block begins and the input read after it, so the two are a block and the
// metric's whole millisecond apart at most.
TEST_CASE("LinkAudio: the buffered metric is what the input holds", "[LinkAudio][stand-in]") {
    EngineFixture fx{standInConfig()};
    REQUIRE_TEST_IS_THE_HOST(fx);
    StandInPeer peer{fx, {{"Main", 2, sine440and880}}};
    REQUIRE(subscribe(fx, kStandIn, "Main") >= 0);
    fx.pumpBlock(blocksIn(fx, 0.5));

    const double blockMs = 1e3 * clockwork_block_size(fx.engine().core()) / 48000.0;
    double metricMs = 0, inputMs = 0, worstApart = 0;
    const uint32_t looks = blocksIn(fx, 1.0);
    for (uint32_t k = 0; k < looks; ++k) {
        fx.pumpBlock();
        const double m = fx.engine().getMetrics().link_audio_buffered_ms.load(std::memory_order_relaxed);
        const double i = 1e3 * inputStatus(fx, "Main").bufferedSeconds;
        metricMs += m;
        inputMs  += i;
        worstApart = std::max(worstApart, std::fabs(m - i));
    }
    INFO("over " << looks << " blocks the metric said " << metricMs / looks << " ms on average, the input "
         << inputMs / looks << " ms, and they were up to " << worstApart << " ms apart");
    CHECK(inputMs / looks > 5.0);
    CHECK(worstApart <= blockMs + 1.0);
}

// The underruns a client reads from the metrics are the engine's inputs':
// what each counted, and what the ones since removed counted before they went.
TEST_CASE("LinkAudio: the engine's underrun metric is its inputs' count, and keeps it once an input goes",
          "[LinkAudio][stand-in]") {
    EngineFixture fx{standInConfig()};
    REQUIRE_TEST_IS_THE_HOST(fx);
    StandInPeer peer{fx, {{"Main", 2, sine440and880}}};
    REQUIRE(subscribe(fx, kStandIn, "Main") >= 0);
    fx.pumpBlock(blocksIn(fx, 1.0));
    peer.stall(0.1);
    fx.pumpBlock(blocksIn(fx, 1.0));

    const auto metric = [&] {
        return uint64_t(fx.engine().getMetrics().link_audio_underruns.load(std::memory_order_relaxed));
    };
    const uint64_t input = inputStatus(fx, "Main").underruns;
    const uint64_t during = metric();
    removeInput(fx, "Main");
    REQUIRE(fx.pollUntil([&] { return countInputs(fx) == 0; }));
    fx.pumpBlock(blocksIn(fx, 0.1));
    const uint64_t after = metric();
    INFO("the input counted " << input << " underruns; the metric said " << during << " while it played and "
         << after << " once it was removed");
    CHECK(input > 0);
    CHECK(during == input);
    CHECK(after == input);
}

// ── Real Link ──────────────────────────────────────────────────────────────

namespace {

// Wait until `peerName`/`channelName` has audio arriving, healthy or not.
// What a hand-pumped engine can wait for: pumped a block at a time between
// polls, slower than a device would, it hears a stream it cannot keep up
// with, and the report says so. Its health is for a device-paced listen.
bool waitForArriving(EngineFixture& fx,
                     const FakeLinkPeerProcess& peer,
                     const std::string& channelName,
                     std::chrono::milliseconds timeout) {
    InputSnapshot snap;
    const bool arriving = fx.pollUntil([&] {
        snap = snapshotInput(fx, peer.options().name, channelName);
        return snap.state == kStateConnected || snap.state == kStateDropout;
    }, static_cast<int>(timeout.count()));
    if (!arriving) {
        UNSCOPED_INFO(peer.account());
        explainInput(fx, "no audio arriving within the wait", snap);
    }
    return arriving;
}

}  // namespace

// A peer in another process, found over Link on this machine, subscribed, and
// heard by a synth: Link's discovery, its wire and its threads, the endpoint
// on a thread of its own and a device's pace. Receive-only: Link Audio is on
// for any visibility but Off, so nothing is published for this.
TEST_CASE("LinkAudio: a peer in another process is found over Link and heard by a synth, without publishing",
          "[Link][LinkAudio][integration]") {
    FakeLinkPeerProcess::Options peerOpts;
    peerOpts.name     = "FakeLive";
    peerOpts.channels = {{"Main", 2, "sine440-880"}};
    FakeLinkPeerProcess peer{peerOpts};
    REQUIRE(peer.ready());

    EngineFixture fx(handPumpedConfig());
    fx.send(osc_test::message("/clockwork/clock/visibility", kThisMachineOnly));
    REQUIRE(waitForChannelVisible(fx, peer, "Main", std::chrono::seconds(30)));
    const int32_t main = subscribe(fx, "FakeLive", "Main");
    REQUIRE(main >= 0);
    REQUIRE(waitForArriving(fx, peer, "Main", std::chrono::seconds(30)));

    REQUIRE(fx.loadSynthDef("stereo_passthrough"));
    {
        osc_test::Builder b;
        b.begin("/s_new") << "stereo_passthrough" << int32_t{1000} << int32_t{0} << int32_t{1}
                          << "in_bus" << inBusFor(fx.engine().core(), main) << "out" << 0.0f;
        fx.send(b.end());
        OscReply r;
        fx.send(osc_test::message("/sync", 42));
        REQUIRE(fx.waitForReply("/synced", r));
    }

    // Rendered at a device's pace until the synth is heard, the output read
    // after each block on the thread that rendered it (renderUntil).
    const auto* outBus = clockwork_audio_out(fx.engine().core());
    REQUIRE(outBus != nullptr);
    const uint32_t n = clockwork_block_size(fx.engine().core());
    float pL = 0.0f, pR = 0.0f, apart = 0.0f;
    renderUntil(fx, [&] {
        const std::vector<float> outL(outBus, outBus + n), outR(outBus + n, outBus + 2 * n);
        pL    = peakAbs(outL);
        pR    = peakAbs(outR);
        apart = maxAbsDiff(outL, outR);
        return pL > 0.01f && pR > 0.01f && apart > 0.01f;
    }, std::chrono::milliseconds(5000));
    if (!(pL > 0.01f && pR > 0.01f && apart > 0.01f))
        explainInput(fx, "the synth was not heard", snapshotInput(fx, "FakeLive", "Main"));
    INFO("output peaks " << pL << " and " << pR << ", L and R up to " << apart << " apart");
    CHECK(pL > 0.01f);
    CHECK(pR > 0.01f);
    CHECK(apart > 0.01f);
}

#endif  // CLOCKWORK_LINK_AUDIO
