/*
 * EngineFixture.cpp
 *
 * The HeadlessDriver ticks process_audio() at real audio rate (2.67ms per
 * 128-sample block at 48kHz).  Worker threads (ReplyReader, DebugReader)
 * drain ring buffers naturally — just like the AudioWorklet + JS workers
 * in the WASM build.  Tests simply send OSC and waitForReply().
 */
#include "EngineFixture.h"
#include "RealtimeThread.h"
#include "DebugTail.h"
#include "ClockworkProcessor.h"
#include "HeadlessDriver.h"
#include "clock/clock_math.h"
#include <catch2/catch_test_macros.hpp>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <fstream>
#include <chrono>
#include <thread>
#include <filesystem>

// Render collected replies one per line. Caller holds mReplyMutex.
static std::string dumpReplies(const std::vector<OscReply>& replies) {
    if (replies.empty()) return "(none)";
    std::string out;
    for (auto& r : replies)
        out += "\n  | " + r.address + " (" + std::to_string(r.raw.size()) + " bytes)";
    return out;
}

ClockworkEngine::Config EngineFixture::defaultConfig() {
    ClockworkEngine::Config cfg;
    cfg.sampleRate        = 48000;
    cfg.bufferSize        = 128;
    cfg.shmSegment           = false;
    setGuestOption(cfg, "numBuffers", 1024);
    setGuestOption(cfg, "maxNodes", 1024);
    setGuestOption(cfg, "maxGraphDefs", 512);
    setGuestOption(cfg, "maxWireBufs", 64);
    cfg.headless          = true;
    // Pin channel counts for the headless test fixture. The live engine
    // uses kAutoChannelCount (-1) so JUCE picks up each device's real
    // channel count, but that's not meaningful without a device — tests
    // want a predictable 2-in / 2-out world to reason against.
    cfg.numOutputChannels = 2;
    cfg.numInputChannels  = 2;
    // Pumped: the test thread renders every block (manualAudioPump) on a
    // clock that is the sample count (freewheelClock), and runs the control
    // pass after it (hostDrivesControl), so a reply is delivered when the test
    // says rather than when the engine's gateway thread wakes: nothing a case
    // asserts can depend on how a thread was scheduled, or on a runner being
    // busy. A case whose claim is about real time asks for the headless
    // driver itself, and is tagged [rt]. SUPERSONIC_TEST_REALTIME=1 runs the
    // whole suite on the driver instead, the shape it once had.
    const char* realtime = std::getenv("SUPERSONIC_TEST_REALTIME");
    const bool pumped = !(realtime && *realtime == '1');
    cfg.manualAudioPump   = pumped;
    cfg.freewheelClock    = pumped;
    cfg.hostDrivesControl = pumped;
    return cfg;
}

ClockworkEngine::Config EngineFixture::driverConfig() {
    ClockworkEngine::Config cfg = defaultConfig();
    cfg.manualAudioPump   = false;
    cfg.freewheelClock    = false;
    cfg.hostDrivesControl = false;
    return cfg;
}

EngineFixture::EngineFixture() {
    init(defaultConfig());
}

EngineFixture::EngineFixture(const ClockworkEngine::Config& cfg) {
    init(cfg);
}

EngineFixture::EngineFixture(const ClockworkEngine::Config& cfg,
                             const std::function<void(ClockworkEngine&)>& beforeBoot) {
    init(cfg, beforeBoot, 1);
}

EngineFixture::EngineFixture(int engines) {
    init(defaultConfig(), {}, engines);
}

EngineFixture::EngineFixture(const ClockworkEngine::Config& cfg, int engines) {
    init(cfg, {}, engines);
}

int EngineFixture::enginesPerFixture() {
    const char* n = std::getenv("SUPERSONIC_TEST_ENGINES");
    const int engines = n ? std::atoi(n) : 1;
    return engines > 1 ? engines : 1;
}

bool EngineFixture::replyIsTheEngineOwn(const std::string& address) {
    return address == "/status.reply" || address.rfind("/clockwork/clock/", 0) == 0;
}

void EngineFixture::init(const ClockworkEngine::Config& cfg,
                         const std::function<void(ClockworkEngine&)>& beforeBoot, int engines) {
    static uint64_t sGeneration = 0;
    mGeneration = ++sGeneration;
    // In manual-pump mode there is no audio-driver thread; the wait primitives
    // drive process_audio on the test thread instead (see pollUntil/waitForReply).
    mManualPump = cfg.manualAudioPump;
    mHostControl = cfg.hostDrivesControl;
    // The fixture runs the control pass after the blocks the test renders, and
    // only then: a driver's engine with it would never deliver a reply.
    INFO("cfg.hostDrivesControl without cfg.manualAudioPump: nothing runs the control pass "
         "(EngineFixture::driverConfig() is the driver's config)");
    REQUIRE((!cfg.hostDrivesControl || cfg.manualAudioPump));
    mSampleRate = cfg.sampleRate;

    // Wire reply/debug callbacks before initialising
    mEngine.onReply = [this](const uint8_t* data, uint32_t size) {
        OscReply r;
        r.address = osc_test::parseAddress(data, size);
        r.raw.assign(data, data + size);
        {
            std::lock_guard<std::mutex> lk(mReplyMutex);
            mLog.push_back(r);
            mReplies.push_back(std::move(r));
        }
        mReplyCv.notify_all();
    };

    // Routed, as the transport decided (before onReply, CallbackTransport::send): recorded here, then handed to
    // the test's observer, if it has set one
    mEngine.onReplyRouted = [this](uint32_t origin, uint32_t route, const uint8_t* data, uint32_t size) {
        {
            std::lock_guard<std::mutex> lk(mReplyMutex);
            mRouted.push_back({origin, route, osc_test::parseAddress(data, size)});
        }
        std::lock_guard<std::mutex> lk(mObserverMutex);
        if (mRoutedObserver) mRoutedObserver(origin, route, data, size);
    };

    mEngine.onDebug = [this](const std::string& msg) {
        debug_tail::push(msg);
        std::lock_guard<std::mutex> lk(mDebugMutex);
        mDebugMessages.push_back(msg);
    };

    if (beforeBoot) beforeBoot(mEngine);
    // Engines beside this one render only when the test renders them, and
    // they render the first's program only from the first's clock origin and
    // random seed: a bundle's time is a sample on an engine's own clock, and
    // a noise is its own seed's.
    ClockworkEngine::Config shared = cfg;
    const bool beside = engines > 1 && cfg.manualAudioPump && cfg.hostDrivesControl;
    if (beside) {
        if (shared.freewheelClock && shared.freewheelOriginNtp == 0.0)
            shared.freewheelOriginNtp = std::chrono::duration<double>(
                std::chrono::system_clock::now().time_since_epoch()).count() + clockwork::kNtpEpochOffset;
        if (guestOption(shared, "randomSeed").empty()) setGuestOption(shared, "randomSeed", 1);
    }
    mEngine.init(shared);
    if (beside) bootCompanions(shared, beforeBoot, engines);

    // Wait for the engine to be ready (a pumped one is pumped by the wait)
    OscReply r;
    send(osc_test::message("/sync", 0));
    waitForReply("/synced", r);

    // Create default group (1) — scsynth only creates root group (0).
    // All SuperCollider clients create group 1 at startup.
    send(osc_test::message("/g_new", 1, 0, 0));
    // Barrier: wait until /g_new has been processed (node tree mirror updated)
    send(osc_test::message("/sync", 1));
    waitForReply("/synced", r);

    // Clear any boot-time debug/reply output so tests start with a clean slate
    clearReplies();
    clearDebugMessages();
    // The replies compared when the fixture goes start here too, on every engine.
    {
        std::lock_guard<std::mutex> lk(mReplyMutex);
        mLog.clear();
    }
    for (auto& c : mCompanions) {
        std::lock_guard<std::mutex> lk(c->mutex);
        c->replies.clear();
    }
}

void EngineFixture::bootCompanions(const ClockworkEngine::Config& cfg,
                                   const std::function<void(ClockworkEngine&)>& beforeBoot, int engines) {
    for (int i = 1; i < engines; ++i) {
        auto c = std::make_unique<Companion>();
        Companion* self = c.get();
        c->engine.onReply = [self](const uint8_t* data, uint32_t size) {
            OscReply r;
            r.address = osc_test::parseAddress(data, size);
            r.raw.assign(data, data + size);
            std::lock_guard<std::mutex> lk(self->mutex);
            self->replies.push_back(std::move(r));
        };
        c->engine.onDebug = [](const std::string& msg) { debug_tail::push(msg); };
        if (beforeBoot) beforeBoot(c->engine);
        c->engine.init(cfg);
        mCompanions.push_back(std::move(c));
    }
}

void EngineFixture::closeCompanions() {
    for (auto& c : mCompanions) {
        c->engine.shutdown();
        c->engine.onReply = nullptr;
        c->engine.onDebug = nullptr;
    }
    mCompanions.clear();
}

EngineFixture::~EngineFixture() {
    // The engines beside answered the same program: the same replies, each
    // the same bytes, but for what is an engine's own. Not after a failure
    // that is unwinding through here: that case has its verdict.
    if (!mCompanions.empty() && !mExpectDivergence && std::uncaught_exceptions() == 0) {
        reportDivergence();
        if (!drainCompanionLanes()) FAIL_CHECK("an engine beside did not run its device lane within 30 s");
        if (const auto differs = replyDivergence()) {
            INFO("replies of the first engine:" << dumpReplies(repliesOf(0)));
            FAIL_CHECK(*differs);
        }
    }
    closeCompanions();
    mEngine.shutdown();
    // shutdown() has stopped the threads that call these; dropping them says so, and leaves nothing holding this
    // fixture while the rest of it goes away.
    mEngine.onReply = nullptr;
    mEngine.onReplyRouted = nullptr;
    mEngine.onDebug = nullptr;
}

// ── Engines beside this one ───────────────────────────────────────────────────

std::vector<OscReply> EngineFixture::repliesOf(int engine) const {
    if (engine == 0) {
        std::lock_guard<std::mutex> lk(mReplyMutex);
        return mLog;
    }
    REQUIRE(engine > 0);
    REQUIRE(static_cast<size_t>(engine) <= mCompanions.size());
    const Companion& c = *mCompanions[static_cast<size_t>(engine) - 1];
    std::lock_guard<std::mutex> lk(c.mutex);
    return c.replies;
}

std::optional<EngineFixture::Divergence> EngineFixture::divergence() const {
    std::lock_guard<std::mutex> lk(mDivergenceMutex);
    return mDivergence;
}

// The replies of an engine as a sorted list of what to compare: the bytes,
// or the address alone for a reply that is the engine's own. Sorted, because
// a reply from an engine's device lane lands when that thread does, among
// the control pass's replies in an order of its own: the engines answer the
// same replies, not in the same order.
static std::vector<std::string> comparable(const std::vector<OscReply>& replies) {
    std::vector<std::string> out;
    out.reserve(replies.size());
    for (const OscReply& r : replies)
        out.push_back(EngineFixture::replyIsTheEngineOwn(r.address) ? r.address
                                                                     : std::string(r.raw.begin(), r.raw.end()));
    std::sort(out.begin(), out.end());
    return out;
}

std::optional<std::string> EngineFixture::replyDivergence() const {
    const std::vector<OscReply> firstReplies = repliesOf(0);
    const std::vector<std::string> first = comparable(firstReplies);
    for (size_t i = 0; i < mCompanions.size(); ++i) {
        const std::vector<OscReply> otherReplies = repliesOf(static_cast<int>(i) + 1);
        const std::vector<std::string> other = comparable(otherReplies);
        const std::string engine = "engine " + std::to_string(i + 2);
        if (first.size() != other.size())
            return engine + " answered " + std::to_string(other.size()) + " replies where the first engine answered "
                 + std::to_string(first.size());
        const auto at = std::mismatch(first.begin(), first.end(), other.begin());
        if (at.first == first.end()) continue;
        // Name the reply by its address: the first string is an address or an
        // OSC packet, whose address is its first string.
        const std::string address = at.first->c_str();
        const std::string answered = at.second->c_str();
        if (address != answered)
            return engine + " answered " + answered + " where the first engine answered " + address;
        return engine + " answered " + address + " with different bytes from the first engine";
    }
    return std::nullopt;
}

bool EngineFixture::drainCompanionLanes() {
    bool drained = true;
    for (auto& c : mCompanions) {
        auto done = std::make_shared<std::promise<void>>();
        auto ran = done->get_future();
        if (!c->engine.postDeviceTask([done] { done->set_value(); })
            || ran.wait_for(std::chrono::seconds(30)) != std::future_status::ready)
            drained = false;
    }
    // What the lanes answered sits in each engine's egress until a control
    // pass hands it on.
    if (mHostControl)
        for (auto& c : mCompanions) c->engine.controlPass();
    return drained;
}

void EngineFixture::compareBlocks() {
    if (mCompanions.empty()) return;
    {
        std::lock_guard<std::mutex> lk(mDivergenceMutex);
        if (mDivergence) return;   // the first is the one that matters
    }
    ClockworkCore* core = mEngine.core();
    const float* want = clockwork_audio_out(core);
    const size_t n = static_cast<size_t>(clockwork_block_size(core)) * 2;
    for (size_t i = 0; i < mCompanions.size(); ++i) {
        const float* got = clockwork_audio_out(mCompanions[i]->engine.core());
        for (size_t s = 0; s < n; ++s) {
            if (std::memcmp(&got[s], &want[s], sizeof(float)) == 0) continue;
            std::lock_guard<std::mutex> lk(mDivergenceMutex);
            mDivergence = Divergence{ mEngine.processor().processCount.value.load(std::memory_order_acquire),
                                      static_cast<int>(i) + 1, s, got[s], want[s] };
            return;
        }
    }
}

void EngineFixture::reportDivergence() {
    if (mExpectDivergence || mDivergenceReported) return;
    const auto d = divergence();
    if (!d) return;
    mDivergenceReported = true;
    FAIL_CHECK("engine " << d->engine + 1 << " rendered block " << d->block << " differently from the first engine: sample "
               << d->sample << " is " << d->got << " where the first engine's is " << d->want);
}

// ── OSC send ──────────────────────────────────────────────────────────────────

void EngineFixture::send(const osc_test::Packet& pkt) {
    send(pkt.ptr(), pkt.size());
}

void EngineFixture::send(const uint8_t* data, uint32_t size) {
    mEngine.sendOSC(data, size);
    for (auto& c : mCompanions) c->engine.sendOSC(data, size);
}

void EngineFixture::sendToFirst(const osc_test::Packet& pkt) {
    mEngine.sendOSC(pkt.ptr(), pkt.size());
}

// ── Reply collection ─────────────────────────────────────────────────────────

bool EngineFixture::waitForReply(const std::string& addr, OscReply& out,
                                  int timeoutMs) {
    auto deadline = std::chrono::steady_clock::now()
                  + std::chrono::milliseconds(timeoutMs);

    auto take = [&]() -> bool {
        std::lock_guard<std::mutex> lk(mReplyMutex);
        for (auto it = mReplies.begin(); it != mReplies.end(); ++it) {
            if (it->address == addr) {
                out = *it;
                mReplies.erase(it);
                return true;
            }
        }
        return false;
    };

    // The test is the host: what it waits for comes from the blocks and passes
    // it runs, so the wait is a count of them.
    if (testIsTheHost()) {
        for (uint32_t owed = blocksOfAudio(timeoutMs);; --owed) {
            if (take()) return true;
            if (!owed) {
                // The caller's assertion expands to a bare "false"; say what we
                // waited for and what did arrive, so a timeout separates "wrong
                // reply" from "no reply at all" without a rerun.
                UNSCOPED_INFO("waitForReply(\"" << addr << "\") gave up after "
                              << timeoutMs << "ms of audio; replies held:" << repliesDump());
                return false;
            }
            pumpBlock();
        }
    }

    // Manual-pump mode with the engine's own gateway: it delivers replies after
    // the audio thread ticks, and nothing ticks unless we pump here on the test
    // thread. Two budgets, audio and wall clock, both spent before giving up
    // (pollUntil says why).
    if (testPumps()) {
        uint32_t owed = blocksOfAudio(timeoutMs);
        while (true) {
            if (owed) { pumpBlock(); --owed; }
            if (take()) return true;
            if (!owed && std::chrono::steady_clock::now() >= deadline) {
                // The caller's assertion expands to a bare "false"; say what we
                // waited for and what did arrive, so a timeout separates "wrong
                // reply" from "no reply at all" without a rerun.
                UNSCOPED_INFO("waitForReply(\"" << addr << "\") timed out after "
                              << timeoutMs << "ms; replies held:" << repliesDump());
                return false;
            }
            // Let the gateway thread (woken by the pump's processCount tick)
            // deliver before the next scan.
            std::this_thread::sleep_for(std::chrono::milliseconds(3));
        }
    }

    std::unique_lock<std::mutex> lk(mReplyMutex);
    while (true) {
        for (auto it = mReplies.begin(); it != mReplies.end(); ++it) {
            if (it->address == addr) {
                out = *it;
                mReplies.erase(it);
                return true;
            }
        }

        if (std::chrono::steady_clock::now() >= deadline) {
            // mReplyMutex is held here, so dump directly rather than via
            // repliesDump() (which takes the same non-recursive lock).
            UNSCOPED_INFO("waitForReply(\"" << addr << "\") timed out after "
                          << timeoutMs << "ms; replies held:" << dumpReplies(mReplies));
            return false;
        }

        mReplyCv.wait_until(lk, deadline);
    }
}

bool EngineFixture::sendAndExpectDone(const osc_test::Packet& pkt,
                                       int timeoutMs) {
    // The OSC address is the packet's first NUL-terminated string; the /done
    // that completes it names that address as its first argument.
    const std::string cmd(reinterpret_cast<const char*>(pkt.ptr()));
    send(pkt);
    return waitForDone(cmd, timeoutMs);
}

// scsynth completes an asynchronous command with `/done <cmd>`; matching the
// address alone took ANY /done for the one awaited, which held only while
// every command completed inline, before the guest's off-thread stages. A
// /done for an earlier command can now arrive later, so the name is checked.
bool EngineFixture::waitForDone(const std::string& cmd, int timeoutMs) {
    return pollUntil([&] {
        std::lock_guard<std::mutex> lk(mReplyMutex);
        for (auto it = mReplies.begin(); it != mReplies.end(); ++it) {
            if (it->address != "/done") continue;
            if (it->parsed().argCount() < 1 || it->parsed().argString(0) != cmd) continue;
            mReplies.erase(it);
            return true;
        }
        return false;
    }, timeoutMs);
}

uint32_t EngineFixture::blocksOfAudio(int ms) const {
    const double frames = double(ms) * double(mSampleRate) / 1000.0;
    return static_cast<uint32_t>(std::ceil(frames / double(clockwork_block_size(mEngine.core()))));
}

bool EngineFixture::waitForBlocks(uint32_t n, int timeoutMs) {
    const uint32_t start =
        mEngine.processor().processCount.value.load(std::memory_order_acquire);
    // Unsigned wrap is fine: (now - start) is the number of blocks rendered
    // since the snapshot regardless of counter wraparound.
    // Pumped: the blocks are rendered here, now, however busy the machine is.
    if (testPumps()) {
        pumpBlock(n);
        return mEngine.processor().processCount.value.load(std::memory_order_acquire) - start >= n;
    }
    return pollUntil([&] {
        return mEngine.processor().processCount.value.load(
                   std::memory_order_acquire) - start >= n;
    }, timeoutMs);
}

std::vector<OscReply> EngineFixture::allReplies() const {
    std::lock_guard<std::mutex> lk(mReplyMutex);
    return mReplies;
}

void EngineFixture::clearReplies() {
    std::lock_guard<std::mutex> lk(mReplyMutex);
    mReplies.clear();
    mRouted.clear();
}

std::vector<RoutedReply> EngineFixture::routedReplies() const {
    std::lock_guard<std::mutex> lk(mReplyMutex);
    return mRouted;
}

void EngineFixture::setRoutedObserver(RoutedObserver fn) {
    std::lock_guard<std::mutex> lk(mObserverMutex);
    mRoutedObserver = std::move(fn);
}

// ── Debug ────────────────────────────────────────────────────────────────────

std::vector<std::string> EngineFixture::debugMessages() const {
    std::lock_guard<std::mutex> lk(mDebugMutex);
    return mDebugMessages;
}

void EngineFixture::clearDebugMessages() {
    std::lock_guard<std::mutex> lk(mDebugMutex);
    mDebugMessages.clear();
}

std::string EngineFixture::debugMessagesDump() const {
    std::lock_guard<std::mutex> lk(mDebugMutex);
    if (mDebugMessages.empty()) return "(none)";
    std::string out;
    for (auto& m : mDebugMessages) out += "\n  | " + m;
    return out;
}

std::string EngineFixture::repliesDump() const {
    std::lock_guard<std::mutex> lk(mReplyMutex);
    return dumpReplies(mReplies);
}

// ── HeadlessDriver control ───────────────────────────────────────────────────

void EngineFixture::stopHeadlessDriver() {
    if (mManualPump) return;   // pumped: there is no driver to stop
    mEngine.mHeadlessDriver->stop();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
}

void EngineFixture::pumpBlock(uint32_t n) {
    if (testPumps()) {
        for (uint32_t i = 0; i < n; ++i) {
            mEngine.pumpAudioBlock();
            for (auto& c : mCompanions) c->engine.pumpAudioBlock();
            compareBlocks();
            hostTurn();
        }
        reportDivergence();
        return;
    }
    // A driver thread or a Device is rendering: a block pumped here would be a
    // second renderer. What a test means by "pump n" with one running is "let
    // n blocks pass" — so wait for its.
    if (!waitForBlocks(n))
        WARN("pumpBlock(" << n << "): the driver rendered no block within the wait");
}

void EngineFixture::pumpCallback(uint32_t frames, const std::function<void()>& afterEachBlock) {
    REQUIRE(testPumps());
    mEngine.pumpAudioCallback(frames, afterEachBlock);
    for (auto& c : mCompanions) c->engine.pumpAudioCallback(frames);
    compareBlocks();   // the callback's last block
    hostTurn();
    reportDivergence();
}

void EngineFixture::restartPump() {
    REQUIRE(testPumps());
    mEngine.restartManualPump();
    for (auto& c : mCompanions) c->engine.restartManualPump();
}

void EngineFixture::setHostWork(std::function<void()> work) {
    // A host of the case's own is bound to the one engine: the engines
    // beside would answer its verbs differently, having no host.
    if (work) closeCompanions();
    mHostWork = std::move(work);
}

void EngineFixture::hostTurn() {
    if (mHostControl) {
        mEngine.controlPass();
        for (auto& c : mCompanions) c->engine.controlPass();
    }
    if (mHostWork) mHostWork();
}

void EngineFixture::waitForDeviceLane() {
    // Sent so far reaches the lane: the block that forwards a control verb, and
    // the pass that hands it over.
    if (testIsTheHost()) pumpBlock();
    // Shared, not on this stack: a lane that never gets to it runs it after
    // the REQUIRE below has thrown.
    auto done = std::make_shared<std::promise<void>>();
    auto ran = done->get_future();
    REQUIRE(mEngine.postDeviceTask([done] { done->set_value(); }));
    REQUIRE(ran.wait_for(std::chrono::seconds(30)) == std::future_status::ready);
    REQUIRE(drainCompanionLanes());
}

// Assertions stay on the test thread: Catch2's aren't safe from another.
EngineFixture::Device::Device(EngineFixture& fx, int sampleRate, uint32_t frames,
                              std::function<void()> afterEachBlock) : mFx(fx) {
    REQUIRE(mFx.mManualPump);
    REQUIRE_FALSE(mFx.mDeviceRendering.load(std::memory_order_relaxed));
    mFx.closeCompanions();   // a device of the case's own renders the one engine
    mFx.mEngine.restartManualPump();
    mFx.mDeviceRendering.store(true, std::memory_order_relaxed);
    mThread = std::thread([this, sampleRate, frames, afterEachBlock = std::move(afterEachBlock)] {
        // A device's callback thread is real-time, and so is this one: an
        // ordinary thread stalled by a build on the same machine would be a
        // device that stopped calling, a dropout of the test's own making.
        clockwork::elevateCurrentThreadToRealtime(double(frames) / double(sampleRate));
        const auto began = std::chrono::steady_clock::now();
        for (uint64_t callback = 0; !mStop.load(std::memory_order_relaxed); ++callback) {
            const auto due = began + std::chrono::nanoseconds(static_cast<int64_t>(
                1e9 * double(callback * frames) / double(sampleRate)));
            std::this_thread::sleep_until(due);
            if (const int64_t stall = mStallNanos.exchange(0, std::memory_order_relaxed); stall > 0)
                std::this_thread::sleep_for(std::chrono::nanoseconds(stall));
            const auto late = std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - due).count();
            if (late > mWorstLateNanos.load(std::memory_order_relaxed))
                mWorstLateNanos.store(late, std::memory_order_relaxed);
            if (late > 1'000'000) {   // a millisecond: the jitter of a timer wake is less
                std::lock_guard<std::mutex> lock(mLateMutex);
                mLateWakes.push_back({due, late / 1e9});
            }
            mFx.mEngine.pumpAudioCallback(frames, afterEachBlock);
            mFx.hostTurn();
        }
    });
}

EngineFixture::Device::~Device() {
    mStop.store(true, std::memory_order_relaxed);
    mThread.join();
    mFx.mDeviceRendering.store(false, std::memory_order_relaxed);
}

EngineFixture::HeldSwap::HeldSwap(EngineFixture& fx, const std::string& deviceName, double sampleRate) : mFx(fx) {
    mFx.closeCompanions();   // the swap is the first engine's alone
    // The hook can run twice in a swap (a retry without the input), so it
    // waits on a signal that, once given, stays given.
    mFx.mEngine.testSwapFailure = [this](bool) -> std::string {
        mEntered.store(true, std::memory_order_release);
        mReleased.wait();
        return "held open";
    };
    mThread = std::thread([this, deviceName, sampleRate] {
        mResult = mFx.mEngine.switchDevice(deviceName, sampleRate);
    });
}

EngineFixture::HeldSwap::~HeldSwap() {
    release();
}

bool EngineFixture::HeldSwap::entered(int timeoutMs) {
    return mFx.waitForThread([this] { return mEntered.load(std::memory_order_acquire); }, timeoutMs);
}

SwapResult EngineFixture::HeldSwap::release() {
    if (!mJoined) {
        mRelease.set_value();
        mThread.join();
        mJoined = true;
        mFx.mEngine.testSwapFailure = nullptr;
    }
    return mResult;
}

// ── Synthdef helpers ─────────────────────────────────────────────────────────

bool EngineFixture::loadSynthDef(const std::string& name) {
    // Look in the shipped synthdef dir first, then the test-only dir
    // (where test fixtures can drop synthdefs that don't belong in the
    // npm package — e.g. stereo_passthrough used by Link Audio tests).
    std::filesystem::path fsPath =
        std::filesystem::path(CLOCKWORK_SYNTHDEFS_DIR) / (name + ".scsyndef");
    if (!std::filesystem::exists(fsPath)) {
        fsPath = std::filesystem::path(CLOCKWORK_TEST_SYNTHDEFS_DIR)
                 / (name + ".scsyndef");
    }
    if (!std::filesystem::exists(fsPath)) return false;

    std::ifstream f(fsPath, std::ios::binary);
    if (!f) return false;

    std::vector<uint8_t> data((std::istreambuf_iterator<char>(f)),
                               std::istreambuf_iterator<char>());
    if (data.empty()) return false;

    auto pkt = osc_test::messageWithBlob("/d_recv", data.data(), data.size());
    return sendAndExpectDone(pkt);
}
