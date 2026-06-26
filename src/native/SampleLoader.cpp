/*
 * SampleLoader.cpp — Background I/O thread for /b_allocRead
 *
 * Matches the WASM architecture:
 *   1. I/O thread decodes audio via libsndfile (off the audio thread)
 *   2. Decoded PCM + metadata are queued as CompletedLoad
 *   3. Audio thread calls installPendingBuffers() to install buffers and
 *      write /done replies to the OUT ring buffer
 *
 * This keeps the OUT ring buffer single-producer (audio thread only).
 */
#include "SampleLoader.h"

#ifdef _WIN32
#include <windows.h>
#endif
#include <filesystem>
#include <sndfile.h>

// scsynth headers for World / SndBuf access
#include "synth/include/plugin_interface/SC_World.h"
#include "synth/include/plugin_interface/SC_SndBuf.h"
#include "synth/include/common/clz.h"
#include "src/buffer_commands.h"

// Shared memory layout + ring buffer types
#include "src/shared_memory.h"

// scsynth allocator (zalloc/zfree use aligned alloc matching free_alig)
#include "synth/server/SC_Prototypes.h"

// oscpack for building /done reply
#include "osc/OscOutboundPacketStream.h"

// Format a diagnostic line and route it through mDebugSink (the engine wires
// that to OscEgress::debug), or stderr before the sink is set.
void SampleLoader::debugLog(const char* fmt, ...) {
    char buf[1024];
    va_list a;
    va_start(a, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, a);
    va_end(a);
    if (n < 0) return;
    uint32_t len = (n < (int)sizeof(buf)) ? (uint32_t)n : (uint32_t)sizeof(buf) - 1;
    if (mDebugSink) mDebugSink(buf, len);
    else { fputs(buf, stderr); fputc('\n', stderr); }
}

extern "C" {
    // Globals from audio_processor.cpp — needed for ring buffer writes.
    // Use `shared_memory` (the arena: public segment when present, else
    // ring_buffer_storage) as the OUT-ring base, consistent with `control`/
    // `metrics` which are derived from the same arena. Writing via
    // ring_buffer_storage directly would target the wrong buffer under a segment.
    extern uint8_t* shared_memory;
    extern ControlPointers* control;
    extern PerformanceMetrics* metrics;
}

// ring_buffer_write defined in audio_processor.cpp (outside any namespace).
// Defaults live on the declaration in audio_processor.cpp; this extern omits
// them (a default may be given in only one declaration per translation unit).
extern bool ring_buffer_write(
    uint8_t* buffer_start,
    uint32_t buffer_size,
    std::atomic<int32_t>* head,
    std::atomic<int32_t>* tail,
    std::atomic<int32_t>* sequence,
    uint32_t route,
    uint32_t source_id,
    const void* data,
    uint32_t data_size,
    std::atomic<uint32_t>* status_flags,
    PerformanceMetrics* metrics
);

// ── Global instance pointer (set by initialise) ─────────────────────────────

static SampleLoader* g_instance = nullptr;

bool native_sample_load(World* world, int bufnum, const char* path,
                        int startFrame, int numFrames) {
    if (!g_instance) return false;
    return g_instance->load(world, bufnum, path, startFrame, numFrames);
}

// ── Platform-aware sf_open (UTF-8 path → wchar on Windows) ──────────────────

static SNDFILE* openSndfile(const char* path, int mode, SF_INFO* info) {
#ifdef _WIN32
    int wlen = MultiByteToWideChar(CP_UTF8, 0, path, -1, nullptr, 0);
    if (wlen > 0) {
        std::vector<wchar_t> wpath(wlen);
        MultiByteToWideChar(CP_UTF8, 0, path, -1, wpath.data(), wlen);
        return sf_wchar_open(wpath.data(), mode, info);
    }
#endif
    return sf_open(path, mode, info);
}

// ── SampleLoader implementation ─────────────────────────────────────────────

SampleLoader::SampleLoader() : Thread("SampleLoader") {}

SampleLoader::~SampleLoader() {
    g_instance = nullptr;
    signalThreadShouldExit();
    mWakeUp.signal();
    stopThread(2000);

    // Free any un-installed completed loads
    int t = mCompTail.load(std::memory_order_relaxed);
    int h = mCompHead.load(std::memory_order_relaxed);
    while (t != h) {
        if (mCompleted[t].data)
            zfree(mCompleted[t].data);
        t = (t + 1) % kMaxPending;
    }
}

void SampleLoader::initialise() {
    g_instance = this;
}

bool SampleLoader::load(World* world, int bufnum, const char* path,
                        int startFrame, int numFrames) {
    int h = mHead.load(std::memory_order_relaxed);
    int next = (h + 1) % kMaxPending;
    if (next == mTail.load(std::memory_order_acquire))
        return false; // queue full

    Request& req = mQueue[h];
    req.world      = world;
    req.bufnum     = bufnum;
    req.startFrame = startFrame;
    req.numFrames  = numFrames;
    req.generation = mGeneration.load(std::memory_order_acquire);
    std::strncpy(req.path, path, sizeof(req.path) - 1);
    req.path[sizeof(req.path) - 1] = '\0';

    mHead.store(next, std::memory_order_release);
    mWakeUp.signal();
    return true;
}

void SampleLoader::run() {
    while (!threadShouldExit()) {
        mWakeUp.wait();
        if (threadShouldExit()) break;

        // Drain all pending requests
        for (;;) {
            int t = mTail.load(std::memory_order_relaxed);
            int h = mHead.load(std::memory_order_acquire);
            if (t == h) break;

            processRequest(mQueue[t]);
            mTail.store((t + 1) % kMaxPending, std::memory_order_release);
        }
    }
}

// ── Pause/resume ──────────────────────────────────────────────────────────

void SampleLoader::pauseLoading() {
    mGeneration.fetch_add(1, std::memory_order_acq_rel);
    mLoadingPaused.store(true, std::memory_order_release);
}

void SampleLoader::resumeLoading() {
    mLoadingPaused.store(false, std::memory_order_release);
}

// ── I/O thread: decode file and enqueue result ──────────────────────────────

void SampleLoader::processRequest(const Request& req) {
    // Check if this request is from a stale generation (pre-cold-swap)
    if (req.generation != mGeneration.load(std::memory_order_acquire)) {
        enqueueCompleted({ req.world, req.bufnum, nullptr, 0, 0, 0, false, req.generation });
        return;
    }

    SF_INFO info = {};
    SNDFILE* sf = openSndfile(req.path, SFM_READ, &info);
    if (!sf) {
        debugLog("[SampleLoader] sf_open failed: %s — %s",
                      req.path, sf_strerror(nullptr));
        enqueueCompleted({ req.world, req.bufnum, nullptr, 0, 0, 0, false, req.generation });
        return;
    }

    // Clamp startFrame / numFrames (same logic as BufAllocReadCmd::Stage2)
    sf_count_t startFrame = req.startFrame;
    sf_count_t numFrames  = req.numFrames;
    if (startFrame < 0)            startFrame = 0;
    if (startFrame > info.frames)  startFrame = info.frames;
    if (numFrames <= 0 || numFrames + startFrame > info.frames)
        numFrames = info.frames - startFrame;

    int numChannels = info.channels;
    int numSamples  = static_cast<int>(numFrames) * numChannels;

    // Allocate buffer with scsynth's aligned allocator (zalloc)
    // so it can be freed by World destruction via free_alig/zfree.
    float* data = static_cast<float*>(zalloc(numSamples, sizeof(float)));
    if (!data) {
        sf_close(sf);
        debugLog("[SampleLoader] zalloc failed for %d samples", numSamples);
        enqueueCompleted({ req.world, req.bufnum, nullptr, 0, 0, 0, false, req.generation });
        return;
    }

    // Read audio data
    sf_seek(sf, startFrame, SEEK_SET);
    sf_count_t framesRead = sf_readf_float(sf, data, numFrames);
    sf_close(sf);

    if (framesRead <= 0) {
        zfree(data);
        debugLog("[SampleLoader] sf_readf_float returned %lld", (long long)framesRead);
        enqueueCompleted({ req.world, req.bufnum, nullptr, 0, 0, 0, false, req.generation });
        return;
    }

    std::string fileName = std::filesystem::path(req.path).filename().string();
    debugLog("[SampleLoader] loaded %s - buf %d, [%lld frames, %d ch, %d Hz], path: %s",
                  fileName.c_str(), req.bufnum, (long long)framesRead,
                  numChannels, info.samplerate, req.path);

    enqueueCompleted({
        req.world,
        req.bufnum,
        data,
        static_cast<int>(framesRead),
        numChannels,
        info.samplerate,
        true,
        req.generation
    });
}

void SampleLoader::enqueueCompleted(CompletedLoad&& load) {
    int h = mCompHead.load(std::memory_order_relaxed);
    int next = (h + 1) % kMaxPending;
    if (next == mCompTail.load(std::memory_order_acquire)) {
        // Queue full — drop the load and free data
        if (load.data) zfree(load.data);
        debugLog("[SampleLoader] completed queue full, dropped buf %d", load.bufnum);
        return;
    }

    mCompleted[h] = std::move(load);
    mCompHead.store(next, std::memory_order_release);
}

// ── Audio thread: install buffers and write replies to OUT ring buffer ───────

void SampleLoader::installPendingBuffers() {
    if (mLoadingPaused.load(std::memory_order_acquire)) return;

    uint32_t currentGen = mGeneration.load(std::memory_order_acquire);

    for (;;) {
        int t = mCompTail.load(std::memory_order_relaxed);
        int h = mCompHead.load(std::memory_order_acquire);
        if (t == h) break;

        const CompletedLoad& load = mCompleted[t];

        if (load.generation != currentGen) {
            // Stale load from a previous generation — discard without freeing.
            // The data was allocated from the supersonic heap which has been
            // reset by FreeAllInternal() during the cold swap. Calling zfree()
            // on abandoned pool memory would corrupt the allocator.
            debugLog("[SampleLoader] discarded stale buf %d (gen %u != %u)",
                          load.bufnum, load.generation, currentGen);
        } else if (load.success) {
            installBuffer(load);
            writeDoneReply(load.bufnum);
        } else {
            writeFailReply(load.bufnum, "/b_allocRead");
        }

        mCompTail.store((t + 1) % kMaxPending, std::memory_order_release);
    }
}

void SampleLoader::installBuffer(const CompletedLoad& load) {
    World* world = load.world;

    // Free previous buffer data before overwriting
    SndBuf* nrtBuf = World_GetNRTBuf(world, load.bufnum);
    float* oldData = nrtBuf->data;

    // Use unified buffer_set_data (no guard samples — native allocates exact size)
    buffer_set_data(world, load.bufnum, load.data, load.numFrames,
                    load.numChannels, load.sampleRate, false);

    zfree(oldData);
}

void SampleLoader::writeDoneReply(int bufnum) {
    if (!control) return;

    char buf[128];
    osc::OutboundPacketStream p(buf, sizeof(buf));
    p << osc::BeginMessage("/done")
      << "/b_allocRead" << bufnum
      << osc::EndMessage;

    ring_buffer_write(
        shared_memory + OUT_BUFFER_START,
        OUT_BUFFER_SIZE,
        &control->out_head,
        &control->out_tail,
        &control->out_sequence,
        EGRESS_BROADCAST_NOTIFY, 0,  // route, source_id (broadcast)
        p.Data(),
        static_cast<uint32_t>(p.Size()),
        &control->status_flags,
        metrics
    );
}

void SampleLoader::writeFailReply(int bufnum, const char* cmdName) {
    if (!control) return;

    char buf[128];
    osc::OutboundPacketStream p(buf, sizeof(buf));
    p << osc::BeginMessage("/fail")
      << cmdName << bufnum
      << osc::EndMessage;

    ring_buffer_write(
        shared_memory + OUT_BUFFER_START,
        OUT_BUFFER_SIZE,
        &control->out_head,
        &control->out_tail,
        &control->out_sequence,
        EGRESS_BROADCAST_NOTIFY, 0,  // route, source_id (broadcast)
        p.Data(),
        static_cast<uint32_t>(p.Size()),
        &control->status_flags,
        metrics
    );
}
