/*
 * EngineFixture.h — Boots a SupersonicEngine for in-process testing.
 *
 * Each test constructs an EngineFixture, which initialises the engine with a
 * real audio device (when available) or falls back to manual audio pumping
 * for headless CI. Replies and debug messages are collected for assertions.
 */
#pragma once

#include "SupersonicEngine.h"
#include "OscTestUtils.h"
#include <catch2/catch_test_macros.hpp>
#include <vector>
#include <mutex>
#include <condition_variable>
#include <string>

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

    // Get all collected replies so far
    std::vector<OscReply> allReplies() const;
    void clearReplies();

    // ── Debug output ───────────────────────────────────────────────────
    std::vector<std::string> debugMessages() const;

    // ── Audio pump (manual mode) ──────────────────────────────────────
    // Synchronously calls process_audio() numBlocks times with wall clock
    // NTP time, then wakes worker threads. Used when the HeadlessDriver
    // is stopped for deterministic testing.
    void pump(int numBlocks = 8);

    // ── Synthdef helpers ───────────────────────────────────────────────
    // Load a .scsyndef file by name (e.g. "sonic-pi-beep")
    bool loadSynthDef(const std::string& name);

    // ── Engine access ──────────────────────────────────────────────────
    SupersonicEngine& engine() { return mEngine; }

private:
    SupersonicEngine mEngine;

    mutable std::mutex       mReplyMutex;
    std::condition_variable  mReplyCv;
    std::vector<OscReply>    mReplies;

    mutable std::mutex       mDebugMutex;
    std::vector<std::string> mDebugMessages;
};
