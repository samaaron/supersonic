/*
 * SampleLoader.h — Background I/O thread for /b_allocRead
 *
 * Matches the WASM architecture: file I/O happens off the audio thread on a
 * dedicated juce::Thread.  Decoded PCM is queued for installation by the
 * audio thread, which installs the buffer and writes the /done reply to the
 * OUT ring buffer — keeping the OUT ring buffer single-producer (audio thread).
 */
#pragma once

#include <juce_core/juce_core.h>
#include <string>
#include <atomic>
#include <array>
#include <functional>

struct World;

class SampleLoader : public juce::Thread {
public:
    SampleLoader();
    ~SampleLoader() override;

    void initialise();

    // Enqueue a load request (called from audio thread — non-blocking).
    // Returns true if enqueued, false if queue is full.
    bool load(World* world, int bufnum, const char* path,
              int startFrame, int numFrames);

    // Called from the AUDIO THREAD to install completed loads and write
    // /done (or /fail) replies to the OUT ring buffer.  This mirrors the
    // WASM architecture where /b_allocPtr is processed on the audio thread.
    void installPendingBuffers();

    void run() override;

    // Wake the I/O thread (used during shutdown to unblock WaitableEvent)
    void wake() { mWakeUp.signal(); }

    // Pause/resume loading (for cold swap — prevents stale World* access)
    void pauseLoading();
    void resumeLoading();

    // SampleLoader runs off the audio thread; the engine wires this sink to
    // OscEgress::debug so its diagnostics ride the locked NRT-out ring. Set
    // before startThread().
    void setDebugSink(std::function<void(const char*, uint32_t)> sink) {
        mDebugSink = std::move(sink);
    }

private:
    // Format + route a diagnostic line to mDebugSink (or stderr if unset).
    void debugLog(const char* fmt, ...);
    std::function<void(const char*, uint32_t)> mDebugSink;

    // ── Request queue (audio thread → I/O thread, SPSC) ─────────────────
    struct Request {
        World*      world      = nullptr;
        int         bufnum     = 0;
        char        path[512]  = {};  // fixed-size, no heap allocation on audio thread
        int         startFrame = 0;
        int         numFrames  = 0;
        uint32_t    generation = 0;
    };

    static constexpr int kMaxPending = 64;
    std::array<Request, kMaxPending> mQueue;
    std::atomic<int> mHead{0};
    std::atomic<int> mTail{0};

    // ── Completed queue (I/O thread → audio thread, SPSC) ───────────────
    struct CompletedLoad {
        World*   world       = nullptr;
        int      bufnum      = 0;
        float*   data        = nullptr;  // heap-allocated PCM (zalloc'd), or nullptr on failure
        int      numFrames   = 0;
        int      numChannels = 0;
        int      sampleRate  = 0;
        bool     success     = false;
        uint32_t generation  = 0;
    };

    std::array<CompletedLoad, kMaxPending> mCompleted;
    std::atomic<int> mCompHead{0};
    std::atomic<int> mCompTail{0};

    void processRequest(const Request& req);
    void enqueueCompleted(CompletedLoad&& load);
    void installBuffer(const CompletedLoad& load);
    void writeDoneReply(int bufnum);
    void writeFailReply(int bufnum, const char* cmdName);

    juce::WaitableEvent mWakeUp;
    std::atomic<bool> mLoadingPaused{false};
    std::atomic<uint32_t> mGeneration{0};
};

// Global hook called by meth_b_allocRead in SC_MiscCmds.cpp.
// Returns true if the request was handled (enqueued to SampleLoader),
// false to fall back to scsynth's synchronous path.
bool native_sample_load(World* world, int bufnum, const char* path,
                        int startFrame, int numFrames);
