/*
 * EngineFixture.h — Boots a ClockworkEngine for in-process testing.
 *
 * Each test constructs an EngineFixture, which initialises the engine with no
 * device, pumped by the test thread on a sample-count clock (defaultConfig):
 * the waits render blocks themselves. A case about real time boots the
 * HeadlessDriver instead, and is tagged [rt]. Replies and debug messages are
 * collected for assertions.
 */
#pragma once

#include "ClockworkEngine.h"
#include "GuestConfigText.h"
#include "OscTestUtils.h"
#include <catch2/catch_test_macros.hpp>
#include <atomic>
#include <vector>
#include <mutex>
#include <condition_variable>
#include <future>
#include <string>
#include <chrono>
#include <thread>
#include <cstdint>

// The guest's options travel as `name=value` text in Config::guestConfig
// (GuestConfigText.h). Two spellings for tests: set one, read one back.
inline void setGuestOption(ClockworkEngine::Config& cfg, const char* name, long value) {
    clockwork::guest_config_text::set(cfg.guestConfig, name, std::to_string(value));
}
inline std::string guestOption(const ClockworkEngine::Config& cfg, const char* name) {
    return clockwork::guest_config_text::get(cfg.guestConfig, name);
}
#include <functional>

// A reply or notification with the routing that decided where it went (ClockworkEngine::onReplyRouted): the
// token it is addressed to, and its EgressRoute.
struct RoutedReply {
    uint32_t    origin;
    uint32_t    route;
    std::string address;
};

struct OscReply {
    std::string          address;
    std::vector<uint8_t> raw;

    // Parse typed arguments from the raw OSC data
    osc_test::ParsedReply parsed() const {
        return osc_test::parseReply(raw.data(), static_cast<uint32_t>(raw.size()));
    }
};

class EngineFixture {
public:
    EngineFixture();
    explicit EngineFixture(const ClockworkEngine::Config& cfg);
    // Given the engine before it boots, for what boot reads: the transport its
    // replies leave by (a front's, wired as the server wires it).
    EngineFixture(const ClockworkEngine::Config& cfg, const std::function<void(ClockworkEngine&)>& beforeBoot);
    ~EngineFixture();

    // ── OSC send (in-process, no UDP) ──────────────────────────────────
    void send(const osc_test::Packet& pkt);
    void send(const uint8_t* data, uint32_t size);

    // ── Reply collection ───────────────────────────────────────────────
    // Wait for a reply whose address matches `addr`. Returns true if found
    // within the timeout. The matched reply is written to `out`.
    bool waitForReply(const std::string& addr, OscReply& out,
                      int timeoutMs = 2000);

    // Convenience: send + wait for /done
    bool sendAndExpectDone(const osc_test::Packet& pkt,
                           int timeoutMs = 2000);

    // ── Progress waits (robust alternative to fixed sleeps) ────────────
    // Wait until the audio thread has rendered `n` more blocks than now,
    // or timeout. Anchors reads of live audio/state to actual DSP progress
    // instead of wall-clock, which a loaded CI runner can't honour — a
    // fixed sleep_for elapses while the audio thread was preempted.
    bool waitForBlocks(uint32_t n, int timeoutMs = 2000);

    // Poll `pred()` until it returns true, or timeout.
    // Generic deadline loop for non-block conditions: a metric atomic
    // reaching a value, a counter draining to zero. Returns the final
    // pred() value (true = condition met before the deadline).
    //
    // Pumped, with the engine's own gateway delivering, the wait has two
    // budgets, and gives up only once both are spent: timeoutMs of audio,
    // pumped a block a turn (what the audio thread answers after so many
    // blocks arrives on audio time, however slow the machine), and timeoutMs
    // of wall clock (what one of the engine's threads delivers once woken
    // needs real time to run). One budget alone was wrong: the wall clock ran
    // out before the audio on a loaded runner, each turn's sleep stretched
    // (SendReply at 2 Hz, macOS arm64, 2026-10-14).
    //
    // When the test is the host (testIsTheHost) there is one budget: what it
    // waits for comes from the blocks and passes it runs, so timeoutMs of
    // audio, a block a turn, and no sleeping.
    template <typename Pred>
    bool pollUntil(Pred pred, int timeoutMs = 2000) {
        if (testIsTheHost()) {
            for (uint32_t owed = blocksOfAudio(timeoutMs);; --owed) {
                if (pred()) return true;
                if (!owed) return false;
                pumpBlock();
            }
        }
        return pollOnBothClocks(pred, timeoutMs);
    }

    // Poll `pred()` for something a thread of the case's own does (a swap
    // held open on one, HeldSwap): the engine pumped as pollUntil pumps it,
    // and timeoutMs of wall clock as well, whoever the host is. That thread
    // runs on real time however the engine is driven; a count of blocks,
    // when the test is the host, pumped 30 s of audio in half a second on a
    // loaded runner and gave up before the thread had been scheduled.
    template <typename Pred>
    bool waitForThread(Pred pred, int timeoutMs = 2000) { return pollOnBothClocks(pred, timeoutMs); }

    // The blocks the engine renders in `ms` of audio, at its rate and block size.
    uint32_t blocksOfAudio(int ms) const;

    // Get all collected replies so far
    std::vector<OscReply> allReplies() const;
    void clearReplies();

    // ── Routed replies: the same traffic, with its origin and route ────
    // Recorded by the fixture from before the engine starts, as onReply's are. A test reads these rather than
    // setting the engine's onReplyRouted itself: a callback over the test's locals outlives them (the engine is
    // destroyed after them, and a notification it delivers in between writes into freed memory: the heap
    // corruption test_load_sample_origin aborted on), and swapping the callback while a reply thread runs it is a
    // race of its own. A reply's routed record is in before waitForReply() sees it (CallbackTransport::send).
    std::vector<RoutedReply> routedReplies() const;
    // A live observer of that traffic, for a test that wires something to it (a front's egress). Swapped under a
    // lock: once setRoutedObserver(nullptr) returns no call is running, and none will start.
    using RoutedObserver = std::function<void(uint32_t origin, uint32_t route, const uint8_t*, uint32_t)>;
    void setRoutedObserver(RoutedObserver fn);

    // ── Debug output ───────────────────────────────────────────────────
    std::vector<std::string> debugMessages() const;
    void clearDebugMessages();

    // One-per-line renderings for Catch2 INFO/CAPTURE: an assertion on a
    // count or on emptiness expands to a bare number, so the contents — the
    // thing that actually identifies the fault — have to be logged alongside.
    std::string debugMessagesDump() const;
    std::string repliesDump() const;


    // ── Synthdef helpers ───────────────────────────────────────────────
    // Load a .scsyndef file by name (e.g. "sonic-pi-beep")
    bool loadSynthDef(const std::string& name);

    // Wait for `/done <cmd>` — the completion of THAT command, not any /done.
    bool waitForDone(const std::string& cmd, int timeoutMs = 2000);

    // A number no other fixture in this process has had: a helper that keeps
    // state per engine (SampleLane.h's pool) keys on it, since a fresh engine
    // may well land on the addresses the last one freed.
    uint64_t generation() const { return mGeneration; }

    // ── Engine access ──────────────────────────────────────────────────
    ClockworkEngine& engine() { return mEngine; }

    // The headless config the default constructor uses. Exposed so tests
    // can tweak one field (e.g. freewheelClock) and pass it to the
    // Config-taking constructor without replicating the whole struct.
    static ClockworkEngine::Config defaultConfig();
    // The same, rendered by the headless driver on a thread of its own, with
    // the engine's gateway delivering its replies: for a case whose claim is
    // about real time, tagged [rt] and run on its own.
    static ClockworkEngine::Config driverConfig();

    // Stop the HeadlessDriver so callers can own process_audio exclusively
    void stopHeadlessDriver();

    // Render `n` audio blocks on the calling (test) thread via
    // ClockworkEngine::pumpAudioBlock(). In manualAudioPump mode (no driver
    // thread) the test thread is the sole audio-thread writer, so a bus snapshot
    // taken between pumps can't race a real-time driver. waitForReply()/pollUntil()
    // pump automatically in this mode, so most tests never call this directly.
    // With a driver or a Device rendering, waits for `n` of its blocks instead.
    // With cfg.hostDrivesControl the test is the host as well: each block is
    // followed by one control pass, as the engine's gateway runs it every block,
    // and the host work (setHostWork).
    void pumpBlock(uint32_t n = 1);

    // Render one device callback of `frames` as a device of that buffer size
    // does — one clock step, then its blocks back to back — running
    // `afterEachBlock` after each. Manual pump only, and no Device rendering.
    // With cfg.hostDrivesControl, one control pass and the host work once the
    // callback is done: when a device's gateway gets to run, its blocks going
    // back to back far faster than the gateway wakes.
    void pumpCallback(uint32_t frames, const std::function<void()>& afterEachBlock = {});

    // The next pump starts the engine's clock afresh, as a device starting
    // does. Manual pump only, and no Device rendering.
    void restartPump();

    // True when constructed with cfg.manualAudioPump — the wait primitives drive
    // the audio thread themselves (see pumpBlock).
    bool manualPump() const { return mManualPump; }

    // The test thread is the one thing that moves the engine: it renders every
    // block (manual pump, no Device) and runs every control pass
    // (cfg.hostDrivesControl, defaultConfig's when pumped), and with them the
    // host work. A wait is then a count of blocks. The one thread of the
    // engine's own left is its device lane (waitForDeviceLane).
    bool testIsTheHost() const { return testPumps() && mHostControl; }

    // What the host runs after each control pass the fixture runs (pumpBlock,
    // pumpCallback, a Device's callback): a front's file jobs and a
    // recording's copying (supersonic::Commands::runPending). Set it before the
    // engine renders and clear it before what it calls goes.
    void setHostWork(std::function<void()> work);

    // Everything sent so far that runs on the engine's device lane has run.
    // The lane is a thread of the engine's own, where the device verbs
    // (/clockwork/devices/...) run, so a case that sends one waits here before
    // it counts blocks for the answer.
    void waitForDeviceLane();

    // A device for a manual-pump engine: a thread rendering a callback of
    // `frames` at each one's due time on the wall clock, catching up after a
    // late wake as a device's buffer would, while the test talks to the engine
    // as a host does. A device is the engine's one renderer, and so is this:
    // while it runs the waits wait for its blocks rather than pump their own,
    // which would be a second thread rendering. The engine's clock starts
    // afresh with it. With cfg.hostDrivesControl it runs the control pass and
    // the host work after each callback, as pumpCallback does. Declared after
    // its fixture, so that it stops first.
    class Device {
    public:
        // `afterEachBlock` runs on the device thread after each block it
        // renders, as a device's callback would run a host's code.
        Device(EngineFixture& fx, int sampleRate, uint32_t frames,
               std::function<void()> afterEachBlock = {});
        ~Device();
        Device(const Device&) = delete;
        Device& operator=(const Device&) = delete;

        // The latest any callback so far woke after its due time, in seconds.
        // A real device's hardware keeps time whatever the host does, and the
        // host underruns; this one catches up instead, so a late wake is heard
        // as a burst of blocks. A test whose premise is a device keeping time
        // reads this to know whether it had one.
        double worstLateSeconds() const { return mWorstLateNanos.load(std::memory_order_relaxed) / 1e9; }

        // Every callback that woke more than a millisecond late, with when it
        // was due: so a test can tell a dropout the machine made, by stalling
        // the device, from one the engine made. Read once the device is gone.
        struct LateWake { std::chrono::steady_clock::time_point due; double lateSeconds; };
        std::vector<LateWake> lateWakes() const { std::lock_guard<std::mutex> lock(mLateMutex); return mLateWakes; }

        // Make the next callback this much late, on purpose: the machine
        // stalling the device, for a test of what is heard when it does.
        void stallNextCallback(std::chrono::nanoseconds by) { mStallNanos.store(by.count(), std::memory_order_relaxed); }

    private:
        EngineFixture&       mFx;
        std::atomic<bool>    mStop{false};
        std::atomic<int64_t> mWorstLateNanos{0};
        std::atomic<int64_t> mStallNanos{0};
        mutable std::mutex   mLateMutex;
        std::vector<LateWake> mLateWakes;
        std::thread          mThread;
    };

    // A cold swap held open on a thread of its own, inside the engine's swap
    // lock, until release(): for a case about what is refused while a swap
    // runs. The engine's testSwapFailure hook holds it, so nothing waits on a
    // sleep a slow machine could outlast, and the swap then fails with
    // "held open". Released and joined when it goes, if not before.
    class HeldSwap {
    public:
        HeldSwap(EngineFixture& fx, const std::string& deviceName, double sampleRate);
        ~HeldSwap();
        HeldSwap(const HeldSwap&) = delete;
        HeldSwap& operator=(const HeldSwap&) = delete;

        // Whether the swap holds the lock: its thread gets there, however long
        // that takes, up to `timeoutMs`.
        bool entered(int timeoutMs = 30000);
        // Let the swap finish; what it returned.
        SwapResult release();

    private:
        EngineFixture&                 mFx;
        std::atomic<bool>              mEntered{false};
        std::promise<void>             mRelease;
        const std::shared_future<void> mReleased = mRelease.get_future().share();
        bool                           mJoined = false;
        SwapResult                     mResult;
        std::thread                    mThread;   // last: it runs against everything above
    };

private:
    // pollUntil's two budgets (above): timeoutMs of audio, pumped a block a
    // turn when the test pumps, and timeoutMs of wall clock.
    template <typename Pred>
    bool pollOnBothClocks(Pred pred, int timeoutMs) {
        const auto deadline = std::chrono::steady_clock::now()
                            + std::chrono::milliseconds(timeoutMs);
        uint32_t owed = testPumps() ? blocksOfAudio(timeoutMs) : 0;
        while (true) {
            // In manual-pump mode nothing advances the engine unless we do it
            // here, on this thread — keeps the test the sole audio-thread writer.
            if (owed) { pumpBlock(); --owed; }
            if (pred()) return true;
            if (!owed && std::chrono::steady_clock::now() >= deadline) return false;
            std::this_thread::sleep_for(std::chrono::milliseconds(testPumps() ? 3 : 2));
        }
    }

    void init(const ClockworkEngine::Config& cfg, const std::function<void(ClockworkEngine&)>& beforeBoot = {});
    // The host's turn after a block or callback: the control pass, when the
    // test drives it, then the host work.
    void hostTurn();
    // The test thread renders: manual pump, and no Device rendering instead.
    bool testPumps() const { return mManualPump && !mDeviceRendering.load(std::memory_order_relaxed); }
    bool             mManualPump = false;  // cfg.manualAudioPump — wait prims pump
    bool             mHostControl = false; // cfg.hostDrivesControl — the pumps pass
    std::function<void()> mHostWork;       // setHostWork
    int              mSampleRate = 48000;  // cfg.sampleRate — blocksOfAudio
    std::atomic<bool> mDeviceRendering{false};  // a Device is the renderer
    uint64_t         mGeneration = 0;      // unique per fixture (see generation())

    mutable std::mutex       mReplyMutex;
    std::condition_variable  mReplyCv;
    std::vector<OscReply>    mReplies;
    std::vector<RoutedReply> mRouted;       // under mReplyMutex

    std::mutex               mObserverMutex;
    RoutedObserver           mRoutedObserver;

    mutable std::mutex       mDebugMutex;
    std::vector<std::string> mDebugMessages;

    // Last, so that it is destroyed first. Its onReply and onDebug hold this fixture and lock the mutexes above;
    // were the engine declared before them it would outlive them, and anything it delivered while it was itself
    // being destroyed would lock a mutex that no longer exists — which is not an error the standard library
    // reports politely: pthread_mutex_lock answers EINVAL, std::mutex::lock throws, no engine thread catches it,
    // and the test binary aborts. Rare, because a reply has to land in that window, and rare is the worst kind.
    ClockworkEngine  mEngine;
};
