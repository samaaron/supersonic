/*
 * EngineFixture.cpp
 *
 * The HeadlessDriver ticks process_audio() at real audio rate (2.67ms per
 * 128-sample block at 48kHz).  Worker threads (ReplyReader, DebugReader)
 * drain ring buffers naturally — just like the AudioWorklet + JS workers
 * in the WASM build.  Tests simply send OSC and waitForReply().
 */
#include "EngineFixture.h"
#include "JuceAudioCallback.h"
#include "SampleLoader.h"
#include <fstream>
#include <chrono>
#include <thread>
#include <filesystem>

static SupersonicEngine::Config defaultConfig() {
    SupersonicEngine::Config cfg;
    cfg.sampleRate    = 48000;
    cfg.bufferSize    = 128;
    cfg.udpPort       = 0;
    cfg.numBuffers    = 1024;
    cfg.maxNodes      = 1024;
    cfg.maxGraphDefs  = 512;
    cfg.maxWireBufs   = 64;
    cfg.headless      = true;
    return cfg;
}

EngineFixture::EngineFixture() {
    init(defaultConfig());
}

EngineFixture::EngineFixture(const SupersonicEngine::Config& cfg) {
    init(cfg);
}

void EngineFixture::init(const SupersonicEngine::Config& cfg) {
    // Wire reply/debug callbacks before initialising
    mEngine.onReply = [this](const uint8_t* data, uint32_t size) {
        OscReply r;
        r.address = osc_test::parseAddress(data, size);
        r.raw.assign(data, data + size);
        {
            std::lock_guard<std::mutex> lk(mReplyMutex);
            mReplies.push_back(std::move(r));
        }
        mReplyCv.notify_all();
    };

    mEngine.onDebug = [this](const std::string& msg) {
        std::lock_guard<std::mutex> lk(mDebugMutex);
        mDebugMessages.push_back(msg);
    };

    mEngine.initialise(cfg);

    // HeadlessDriver is now ticking — wait for the engine to be ready
    OscReply r;
    mEngine.sendOsc(osc_test::message("/sync", 0).ptr(),
                    osc_test::message("/sync", 0).size());
    waitForReply("/synced", r);

    // Create default group (1) — scsynth only creates root group (0).
    // All SuperCollider clients create group 1 at startup.
    send(osc_test::message("/g_new", 1, 0, 0));
    // Barrier: wait until /g_new has been processed (node tree mirror updated)
    auto syncPkt = osc_test::message("/sync", 1);
    mEngine.sendOsc(syncPkt.ptr(), syncPkt.size());
    waitForReply("/synced", r);

    // Clear any boot-time debug/reply output so tests start with a clean slate
    clearReplies();
    clearDebugMessages();
}

EngineFixture::~EngineFixture() {
    mEngine.shutdown();
}

// ── OSC send ──────────────────────────────────────────────────────────────────

void EngineFixture::send(const osc_test::Packet& pkt) {
    send(pkt.ptr(), pkt.size());
}

void EngineFixture::send(const uint8_t* data, uint32_t size) {
    mEngine.sendOsc(data, size);
}

// ── Reply collection ─────────────────────────────────────────────────────────

bool EngineFixture::waitForReply(const std::string& addr, OscReply& out,
                                  int timeoutMs) {
    auto deadline = std::chrono::steady_clock::now()
                  + std::chrono::milliseconds(timeoutMs);

    std::unique_lock<std::mutex> lk(mReplyMutex);
    while (true) {
        for (auto it = mReplies.begin(); it != mReplies.end(); ++it) {
            if (it->address == addr) {
                out = *it;
                mReplies.erase(it);
                return true;
            }
        }

        if (std::chrono::steady_clock::now() >= deadline)
            return false;

        mReplyCv.wait_until(lk, deadline);
    }
}

bool EngineFixture::sendAndExpectDone(const osc_test::Packet& pkt,
                                       int timeoutMs) {
    send(pkt);
    OscReply r;
    return waitForReply("/done", r, timeoutMs);
}

std::vector<OscReply> EngineFixture::allReplies() const {
    std::lock_guard<std::mutex> lk(mReplyMutex);
    return mReplies;
}

void EngineFixture::clearReplies() {
    std::lock_guard<std::mutex> lk(mReplyMutex);
    mReplies.clear();
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

// ── HeadlessDriver control ───────────────────────────────────────────────────

void EngineFixture::stopHeadlessDriver() {
    mEngine.mHeadlessDriver.signalThreadShouldExit();
    mEngine.mHeadlessDriver.stopThread(2000);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
}

// ── Synthdef helpers ─────────────────────────────────────────────────────────

bool EngineFixture::loadSynthDef(const std::string& name) {
    std::string path = std::string(SUPERSONIC_SYNTHDEFS_DIR) + "/" + name + ".scsyndef";

    // Normalise path separators for the platform
    std::filesystem::path fsPath(path);
    if (!std::filesystem::exists(fsPath)) return false;

    std::ifstream f(fsPath, std::ios::binary);
    if (!f) return false;

    std::vector<uint8_t> data((std::istreambuf_iterator<char>(f)),
                               std::istreambuf_iterator<char>());
    if (data.empty()) return false;

    auto pkt = osc_test::messageWithBlob("/d_recv", data.data(), data.size());
    return sendAndExpectDone(pkt);
}
