// Behaviour when the default (JUCE) engine has no audio device.
//
// Contract: no device → the engine is IDLE (no audio source), reports it, and
// the watchdog keeps trying to open one so it self-heals when a device appears.
// Headless is an EXPLICIT mode only (Config::headless / manualAudioPump — the
// test harness and future non-JUCE backends).
#include <catch2/catch_test_macros.hpp>
#include "EngineFixture.h"
#include "OscTestUtils.h"
#include "SupersonicEngine.h"
#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>
#include <vector>

namespace {

// Boot config that mirrors how Sonic Pi launches supersonic at runtime:
// non-headless (a real device manager exists), 2-channel output, no input,
// 48 kHz, no UDP port (we send OSC in-process via sendOSC to keep the test
// hermetic).
SupersonicEngine::Config nonHeadlessTestConfig() {
    SupersonicEngine::Config cfg;
    cfg.sampleRate        = 48000;
    cfg.bufferSize        = 128;
    cfg.udpPort           = 0;
    cfg.numBuffers        = 1024;
    cfg.maxNodes          = 1024;
    cfg.maxGraphDefs      = 512;
    cfg.maxWireBufs       = 64;
    cfg.headless          = false;   // mDeviceManager will be created; NOT headless
    cfg.numOutputChannels = 2;
    cfg.numInputChannels  = 0;
    return cfg;
}

// EngineFixture constructs and initialises atomically, which is too late
// to set testForceNoCurrentDeviceAfterInit. This harness sets the flag
// before init().
class NoDeviceEngine {
public:
    NoDeviceEngine() {
        mEngine.onReply = [this](const uint8_t* data, uint32_t size) {
            std::lock_guard<std::mutex> lk(mMutex);
            Reply r;
            r.address = osc_test::parseAddress(data, size);
            r.raw.assign(data, data + size);
            mReplies.push_back(std::move(r));
        };
        mEngine.onDebug = [](const std::string&) {};
        mEngine.testForceNoCurrentDeviceAfterInit = true;
    }

    ~NoDeviceEngine() { mEngine.shutdown(); }

    void init(const SupersonicEngine::Config& cfg) { mEngine.init(cfg); }
    SupersonicEngine& engine() { return mEngine; }

private:
    struct Reply { std::string address; std::vector<uint8_t> raw; };
    SupersonicEngine mEngine;
    std::mutex mMutex;
    std::vector<Reply> mReplies;
};

uint32_t processCount(SupersonicEngine& e) {
    return e.audioCallback().processCount.load(std::memory_order_acquire);
}

bool pollUntil(std::function<bool()> pred, int timeoutMs) {
    const auto deadline = std::chrono::steady_clock::now()
                        + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return pred();
}

} // namespace

// ── The core contract: no device → idle waiting state, never a silent driver ──

TEST_CASE("NoAudioDevice: boot with no device lands in the waiting state, not a driver",
          "[NoAudioDevice]") {
    NoDeviceEngine harness;
    REQUIRE_NOTHROW(harness.init(nonHeadlessTestConfig()));

    // The engine boots (so it can accept device-switch / prefs OSC and recover)
    // but has NO audio source.
    CHECK(harness.engine().isRunning());
    CHECK(harness.engine().audioSource() == SupersonicEngine::AudioSource::None);
    CHECK(harness.engine().audioSource() != SupersonicEngine::AudioSource::Headless);
    CHECK(harness.engine().waitingForAudioDevice());
}

TEST_CASE("NoAudioDevice: the waiting state does not fake playback (no ticks)",
          "[NoAudioDevice]") {
    NoDeviceEngine harness;
    harness.init(nonHeadlessTestConfig());   // watchdog off in this config

    // With no device and no source, process_audio must not be running. A frozen
    // processCount is the machine-checkable form of "not faking a session".
    const uint32_t before = processCount(harness.engine());
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    CHECK(processCount(harness.engine()) == before);
}

TEST_CASE("NoAudioDevice: device manager survives and reports no current device",
          "[NoAudioDevice]") {
    NoDeviceEngine harness;
    harness.init(nonHeadlessTestConfig());

    // mDeviceManager STAYS alive, so currentDevice() reports an empty device
    // (no crash, no fake) and a device can be opened later — by the watchdog's
    // recovery, or a device-switch once audio is draining again.
    auto dev = harness.engine().currentDevice();
    CHECK(dev.name.empty());
    CHECK(dev.activeSampleRate == 0.0);
}

TEST_CASE("NoAudioDevice: shutdown from the waiting state is clean",
          "[NoAudioDevice]") {
    NoDeviceEngine harness;
    harness.init(nonHeadlessTestConfig());
    REQUIRE(harness.engine().isRunning());
    // Destructor calls shutdown(); no source thread to join, must not hang.
}

// ── Headless is explicit-only: the two paths now DIVERGE ──────────────────────

TEST_CASE("NoAudioDevice: explicit headless ticks; no-device non-headless stays idle",
          "[NoAudioDevice]") {
    {
        // Explicit headless (Config::headless=true) — tests / non-JUCE backends.
        EngineFixture fix;  // default fixture sets headless=true
        CHECK(fix.engine().audioSource() == SupersonicEngine::AudioSource::Headless);
        CHECK(pollUntil([&] { return processCount(fix.engine()) > 20; }, 3000));
    }
    {
        // Default engine, no device — idle, no headless, no ticks.
        NoDeviceEngine harness;
        harness.init(nonHeadlessTestConfig());
        CHECK(harness.engine().audioSource() != SupersonicEngine::AudioSource::Headless);
        CHECK(harness.engine().audioSource() == SupersonicEngine::AudioSource::None);
    }
}

// ── The watchdog self-heals out of the waiting state ──────────────────────────

TEST_CASE("NoAudioDevice: the watchdog keeps trying to open a device",
          "[NoAudioDevice]") {
    NoDeviceEngine harness;
    auto cfg = nonHeadlessTestConfig();
    cfg.callbackWatchdog = true;
    cfg.watchdogStallMs  = 250;
    cfg.watchdogPollMs   = 50;
    harness.init(cfg);

    // The watchdog fires recovery to open a device. On a machine with hardware
    // it climbs back to a real source; on a headless CI runner the attempts keep
    // failing — either way an attempt happens within the window.
    CHECK(pollUntil([&] {
        return harness.engine().watchdogRecoveryCount() >= 1
            || harness.engine().audioSource() == SupersonicEngine::AudioSource::RealCallback;
    }, 3000));

    CHECK(harness.engine().audioSource() != SupersonicEngine::AudioSource::Headless);
}

// ── Regression (W1): the waiting branch must honour an in-flight swap ──────────

// A normal switchDevice holds mSwapMutex across a transient mActiveSource==None
// window (stopAudioSource → cold swap → startAudioSource). The watchdog's benign
// block already treats "swap in flight" as a reason to skip a recovery, but the
// waitingForAudioDevice() branch ABOVE it does not. During that window it fires a
// recovery which recreates the device manager and reopens the SYSTEM DEFAULT —
// silently reverting the device the switch just selected.
//
// Deterministic without hardware: hold the swap gate from BEFORE the watchdog
// starts, so every recovery attempt is refused the gate and takes recoverAudio's
// "device busy" path, which (by design) does NOT stamp the cooldown — so the
// cooldown can never mask the defect after a first attempt. A correct watchdog
// launches ZERO recoveries while a swap is in flight.
TEST_CASE("NoAudioDevice: watchdog defers recovery while a device swap holds the gate",
          "[NoAudioDevice]") {
    NoDeviceEngine harness;

    // Hold the gate before init() spins up the watchdog thread.
    auto gate = harness.engine().testHoldSwapGate();

    auto cfg = nonHeadlessTestConfig();
    cfg.callbackWatchdog = true;
    cfg.watchdogStallMs  = 100;
    cfg.watchdogPollMs   = 20;
    harness.init(cfg);

    REQUIRE(harness.engine().waitingForAudioDevice());

    // Several poll windows elapse with the swap in flight the whole time.
    std::this_thread::sleep_for(std::chrono::milliseconds(400));

    // No recovery should have launched: a swap is in flight, exactly the state
    // the benign block skips. (Fails today — the waiting branch fires anyway.)
    CHECK(harness.engine().watchdogRecoveryCount() == 0);

    gate.unlock();
}
