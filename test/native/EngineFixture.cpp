/*
 * EngineFixture.cpp
 */
#include "EngineFixture.h"
#include "JuceAudioCallback.h"
#include "SampleLoader.h"
#include <fstream>
#include <chrono>
#include <thread>
#include <filesystem>

extern "C" {
    bool process_audio(double current_time, uint32_t active_output_channels,
                       uint32_t active_input_channels);
}

static constexpr double NTP_EPOCH_OFFSET = 2208988800.0;

EngineFixture::EngineFixture() {
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

    SupersonicEngine::Config cfg;
    cfg.sampleRate    = 48000;
    cfg.bufferSize    = 128;
    cfg.udpPort       = 0;
    cfg.numBuffers    = 1024;
    cfg.maxNodes      = 1024;
    cfg.maxGraphDefs  = 512;
    cfg.maxWireBufs   = 64;
    cfg.headless      = true;
    mEngine.initialise(cfg);

    // Stop the HeadlessDriver — tests use manual pump() for deterministic control
    mEngine.mHeadlessDriver.signalThreadShouldExit();
    mEngine.mHeadlessDriver.stopThread(1000);

    // Pump a few blocks so the engine is ready to process commands
    pump(16);

    // Create default group (1) — scsynth only creates root group (0).
    // All SuperCollider clients create group 1 at startup.
    send(osc_test::message("/g_new", 1, 0, 0));
    pump(8);
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
    // Pump audio so the message gets processed
    pump(4);
}

// ── Reply collection ─────────────────────────────────────────────────────────

bool EngineFixture::waitForReply(const std::string& addr, OscReply& out,
                                  int timeoutMs) {
    auto deadline = std::chrono::steady_clock::now()
                  + std::chrono::milliseconds(timeoutMs);

    // Pump a few blocks to kick things off
    pump(4);

    std::unique_lock<std::mutex> lk(mReplyMutex);
    while (true) {
        for (auto it = mReplies.begin(); it != mReplies.end(); ++it) {
            if (it->address == addr) {
                out = *it;
                mReplies.erase(it);
                return true;
            }
        }

        // Wait briefly for a cv notification (e.g. from ReplyReader), then
        // pump again.  This handles async replies (SampleLoader) that need
        // installPendingBuffers() to run before the reply hits the OUT buffer.
        auto waitEnd = std::min(deadline,
            std::chrono::steady_clock::now() + std::chrono::milliseconds(50));
        mReplyCv.wait_until(lk, waitEnd);

        if (std::chrono::steady_clock::now() >= deadline) {
            // Final pump attempt before giving up
            lk.unlock();
            pump(8);
            lk.lock();
            for (auto it = mReplies.begin(); it != mReplies.end(); ++it) {
                if (it->address == addr) {
                    out = *it;
                    mReplies.erase(it);
                    return true;
                }
            }
            return false;
        }

        // Not at deadline — pump and loop
        lk.unlock();
        pump(4);
        lk.lock();
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

// ── Audio pump ───────────────────────────────────────────────────────────────

void EngineFixture::pump(int numBlocks) {
    for (int i = 0; i < numBlocks; i++) {
        mEngine.mSampleLoader.installPendingBuffers();
        double wallNTP = static_cast<double>(juce::Time::currentTimeMillis()) * 0.001
                         + NTP_EPOCH_OFFSET;
        process_audio(wallNTP,
                      static_cast<uint32_t>(mEngine.mCurrentConfig.numOutputChannels),
                      static_cast<uint32_t>(mEngine.mCurrentConfig.numInputChannels));
    }
    mEngine.mAudioCallback.processCount.fetch_add(1, std::memory_order_release);
    mEngine.mAudioCallback.processCount.notify_all();
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
