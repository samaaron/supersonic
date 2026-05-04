// Regression tests for issue #3526: when JUCE's device manager exists but
// has no current device (e.g. ALSA returning "no channels" against
// PipeWire's default sink), the engine must fall back to the headless
// driver so process_audio still ticks and OSC commands drain.
//
// testForceNoCurrentDeviceAfterInit reproduces this deterministically by
// opening a real device, then closing it before the audio-source decision.
#include <catch2/catch_test_macros.hpp>
#include "EngineFixture.h"
#include "OscTestUtils.h"
#include "SupersonicEngine.h"
#include <atomic>
#include <chrono>
#include <fstream>
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
    cfg.headless          = false;   // mDeviceManager will be created
    cfg.numOutputChannels = 2;
    cfg.numInputChannels  = 0;
    return cfg;
}

// EngineFixture constructs and initialises atomically, which is too late
// to set testForceNoCurrentDeviceAfterInit. This harness sets the flag
// before init().
class FailedInitEngine {
public:
    FailedInitEngine() {
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

    ~FailedInitEngine() { mEngine.shutdown(); }

    void init(const SupersonicEngine::Config& cfg) {
        mEngine.init(cfg);
    }

    SupersonicEngine& engine() { return mEngine; }

    bool waitForReplyMatching(const std::string& address, int timeoutMs) {
        auto deadline = std::chrono::steady_clock::now()
                      + std::chrono::milliseconds(timeoutMs);
        while (std::chrono::steady_clock::now() < deadline) {
            {
                std::lock_guard<std::mutex> lk(mMutex);
                for (auto& r : mReplies)
                    if (r.address == address) return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        return false;
    }

private:
    struct Reply {
        std::string address;
        std::vector<uint8_t> raw;
    };
    SupersonicEngine mEngine;
    std::mutex mMutex;
    std::vector<Reply> mReplies;
};

uint32_t processCount(SupersonicEngine& e) {
    return e.audioCallback().processCount.load(std::memory_order_acquire);
}

} // namespace

// ── Boot path ───────────────────────────────────────────────────────────────

TEST_CASE("HeadlessFallback: boot completes when device init fails",
          "[HeadlessFallback]") {
    FailedInitEngine harness;
    auto cfg = nonHeadlessTestConfig();

    // Should not throw. Before the fix this could hang for 5 s in
    // waitForFirstAudioTick and continue with no audio source running;
    // after the fix it boots in <1 s via the headless driver.
    REQUIRE_NOTHROW(harness.init(cfg));
    REQUIRE(harness.engine().isRunning());
}

TEST_CASE("HeadlessFallback: process_audio runs after failed device init",
          "[HeadlessFallback]") {
    FailedInitEngine harness;
    harness.init(nonHeadlessTestConfig());

    // The bug: process_audio was never called because neither the audio
    // callback (no device) nor the headless driver (not started) was
    // ticking. Snapshot the count and verify it advances under its own
    // power within a generous window. The headless driver runs at audio
    // rate (~3 ms / 128-sample block @ 48 kHz) so we should see hundreds
    // of ticks in 200 ms even on a slow CI machine.
    uint32_t before = processCount(harness.engine());
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    uint32_t after = processCount(harness.engine());

    INFO("process_audio invocations during 200ms window: " << (after - before));
    REQUIRE(after > before);
    // Sanity-check the rate: at 48 kHz / 128 samples we expect ~75 ticks
    // in 200 ms. Allow plenty of slack for thread scheduling on busy CI,
    // but flag if we're seeing only one or two ticks (suggests the
    // driver isn't running at audio rate).
    CHECK((after - before) > 10);
}

TEST_CASE("HeadlessFallback: /sync round-trips through process_audio",
          "[HeadlessFallback]") {
    FailedInitEngine harness;
    harness.init(nonHeadlessTestConfig());

    // /sync triggers /synced via the IN/OUT ring buffers, which only works
    // if process_audio is actually ticking. Failure here matches the
    // user-visible symptom (Spider's with_done_sync timeout).
    auto pkt = osc_test::message("/sync", 4242);
    harness.engine().sendOSC(pkt.ptr(), pkt.size());

    REQUIRE(harness.waitForReplyMatching("/synced", /*timeoutMs=*/2000));
}

TEST_CASE("HeadlessFallback: /d_recv reaches /done (the user-reported failure)",
          "[HeadlessFallback]") {
    // This is the exact OSC traffic that times out in issue #3526:
    // Spider sends synthdef bytes via /d_recv (or /d_loadDir) and waits
    // for /done with a 5 s deadline (with_done_sync). Replicate the
    // path with a real synthdef from the test asset directory.
    FailedInitEngine harness;
    harness.init(nonHeadlessTestConfig());

    std::string path = std::string(SUPERSONIC_SYNTHDEFS_DIR) + "/sonic-pi-beep.scsyndef";
    std::ifstream f(path, std::ios::binary);
    if (!f) SKIP("sonic-pi-beep.scsyndef not available at " << path);

    std::vector<uint8_t> data((std::istreambuf_iterator<char>(f)),
                               std::istreambuf_iterator<char>());
    REQUIRE_FALSE(data.empty());

    auto pkt = osc_test::messageWithBlob("/d_recv", data.data(), data.size());
    harness.engine().sendOSC(pkt.ptr(), pkt.size());

    // Match Spider's 5 s deadline (with_done_sync). On a working engine
    // this returns in <50 ms via the headless driver.
    REQUIRE(harness.waitForReplyMatching("/done", /*timeoutMs=*/5000));
}

// ── State invariants ────────────────────────────────────────────────────────

TEST_CASE("HeadlessFallback: device manager survives but reports no current device",
          "[HeadlessFallback]") {
    FailedInitEngine harness;
    harness.init(nonHeadlessTestConfig());

    // The contract: in the failed-init state, mDeviceManager STAYS alive
    // so the user can later call switchDevice() to pick a working device
    // via the GUI prefs panel. currentDevice() reports empty because no
    // device is currently open.
    auto dev = harness.engine().currentDevice();
    CHECK(dev.name.empty());
    CHECK(dev.activeSampleRate == 0.0);
}

// ── Shutdown ────────────────────────────────────────────────────────────────

TEST_CASE("HeadlessFallback: shutdown after failed-init boot is clean",
          "[HeadlessFallback]") {
    // Verifies the headless thread we started in the fallback path is
    // properly joined on shutdown. Pre-fix the fallback path didn't
    // start the headless thread at all so this was vacuously true; the
    // refactor MUST keep shutdown clean for the new boot mode.
    FailedInitEngine harness;
    harness.init(nonHeadlessTestConfig());

    REQUIRE(harness.engine().isRunning());
    // Destructor calls shutdown(). If the thread isn't joined cleanly
    // we'll hang or crash here.
}

// ── Equivalence with explicit headless mode ─────────────────────────────────

TEST_CASE("HeadlessFallback: process_audio rate matches cfg.headless==true mode",
          "[HeadlessFallback]") {
    // Both pathways (explicit headless + failed-init fallback) end up
    // running the same HeadlessDriver thread. Sanity-check that the
    // tick rate is comparable so we don't accidentally end up with a
    // crippled fallback driver later.
    auto countTicksIn200ms = [](SupersonicEngine& e) {
        uint32_t before = e.audioCallback().processCount.load(std::memory_order_acquire);
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        uint32_t after  = e.audioCallback().processCount.load(std::memory_order_acquire);
        return after - before;
    };

    uint32_t headlessTicks = 0;
    {
        EngineFixture fix;  // cfg.headless = true (default fixture)
        headlessTicks = countTicksIn200ms(fix.engine());
    }

    uint32_t fallbackTicks = 0;
    {
        FailedInitEngine harness;
        harness.init(nonHeadlessTestConfig());
        fallbackTicks = countTicksIn200ms(harness.engine());
    }

    INFO("headless=" << headlessTicks << " fallback=" << fallbackTicks);
    CHECK(headlessTicks > 10);
    CHECK(fallbackTicks > 10);
    // Within 50% of each other (same driver, same rate, similar load).
    int diff = static_cast<int>(headlessTicks) - static_cast<int>(fallbackTicks);
    if (diff < 0) diff = -diff;
    CHECK(diff < static_cast<int>(headlessTicks) / 2 + 5);
}
