/*
 * EngineFixture.h — Boots a ClockworkEngine for in-process testing.
 *
 * Each test constructs an EngineFixture, which initialises the engine in
 * headless mode with manual audio pumping via the HeadlessDriver.
 * Replies and debug messages are collected for assertions.
 */
#pragma once

#include "ClockworkEngine.h"
#include "OscTestUtils.h"
#include <catch2/catch_test_macros.hpp>
#include <vector>
#include <mutex>
#include <condition_variable>
#include <string>
#include <chrono>
#include <thread>
#include <cstdint>
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

    // Poll `pred()` (every ~2 ms) until it returns true, or timeout.
    // Generic deadline loop for non-block conditions: a metric atomic
    // reaching a value, a counter draining to zero. Returns the final
    // pred() value (true = condition met before the deadline).
    template <typename Pred>
    bool pollUntil(Pred pred, int timeoutMs = 2000) {
        const auto deadline = std::chrono::steady_clock::now()
                            + std::chrono::milliseconds(timeoutMs);
        while (true) {
            // In manual-pump mode nothing advances the engine unless we do it
            // here, on this thread — keeps the test the sole audio-thread writer.
            if (mManualPump) pumpBlock();
            if (pred()) return true;
            if (std::chrono::steady_clock::now() >= deadline) return false;
            std::this_thread::sleep_for(std::chrono::milliseconds(mManualPump ? 3 : 2));
        }
    }

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

    // Stop the HeadlessDriver so callers can own process_audio exclusively
    void stopHeadlessDriver();

    // Render `n` audio blocks on the calling (test) thread via
    // ClockworkEngine::pumpAudioBlock(). In manualAudioPump mode (no driver
    // thread) the test thread is the sole audio-thread writer, so a bus snapshot
    // taken between pumps can't race a real-time driver. waitForReply()/pollUntil()
    // pump automatically in this mode, so most tests never call this directly.
    void pumpBlock(uint32_t n = 1);

    // True when constructed with cfg.manualAudioPump — the wait primitives drive
    // the audio thread themselves (see pumpBlock).
    bool manualPump() const { return mManualPump; }

private:
    void init(const ClockworkEngine::Config& cfg);
    bool             mManualPump = false;  // cfg.manualAudioPump — wait prims pump
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
