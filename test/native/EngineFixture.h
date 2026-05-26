/*
 * EngineFixture.h — Boots a SupersonicEngine for in-process testing.
 *
 * Each test constructs an EngineFixture, which initialises the engine in
 * headless mode with manual audio pumping via the HeadlessDriver.
 * Replies and debug messages are collected for assertions.
 */
#pragma once

#include "SupersonicEngine.h"
#include "OscTestUtils.h"
#include <catch2/catch_test_macros.hpp>
#include <vector>
#include <mutex>
#include <condition_variable>
#include <string>
#include <chrono>
#include <thread>
#include <cstdint>

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
    explicit EngineFixture(const SupersonicEngine::Config& cfg);
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
            if (pred()) return true;
            if (std::chrono::steady_clock::now() >= deadline) return false;
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    }

    // Get all collected replies so far
    std::vector<OscReply> allReplies() const;
    void clearReplies();

    // ── Debug output ───────────────────────────────────────────────────
    std::vector<std::string> debugMessages() const;
    void clearDebugMessages();


    // ── Synthdef helpers ───────────────────────────────────────────────
    // Load a .scsyndef file by name (e.g. "sonic-pi-beep")
    bool loadSynthDef(const std::string& name);

    // ── Engine access ──────────────────────────────────────────────────
    SupersonicEngine& engine() { return mEngine; }

    // The headless config the default constructor uses. Exposed so tests
    // can tweak one field (e.g. freewheelClock) and pass it to the
    // Config-taking constructor without replicating the whole struct.
    static SupersonicEngine::Config defaultConfig();

    // Stop the HeadlessDriver so callers can own process_audio exclusively
    void stopHeadlessDriver();

private:
    void init(const SupersonicEngine::Config& cfg);
    SupersonicEngine mEngine;

    mutable std::mutex       mReplyMutex;
    std::condition_variable  mReplyCv;
    std::vector<OscReply>    mReplies;

    mutable std::mutex       mDebugMutex;
    std::vector<std::string> mDebugMessages;
};
