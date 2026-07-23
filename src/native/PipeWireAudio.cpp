/*
 * PipeWireAudio.cpp — native PipeWire audio backend (Linux)
 *
 * Structure mirrors JUCE's JACK backend: a process-lifetime connection to the
 * daemon (PipeWireSystem), a device type that snapshots the node graph, and a
 * device that wraps a playback/capture pw_stream pair. Streams negotiate
 * F32 planar, which maps 1:1 onto the float** planes the JUCE callback
 * expects — no interleaving on the audio path.
 *
 * Threading model:
 *  - Registry/state events run on the pw_thread_loop thread with the loop
 *    lock held; we touch shared registry state only under that lock.
 *  - process callbacks run on PipeWire's RT data thread
 *    (PW_STREAM_FLAG_RT_PROCESS) and stay malloc- and syscall-free apart
 *    from the dequeue/queue pair, matching the JACK backend's use of a
 *    CriticalSection around the JUCE callback pointer.
 */

#if defined(__linux__) && defined(SUPERSONIC_PIPEWIRE)

#include "PipeWireAudio.h"

#include <dlfcn.h>
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <type_traits>
#include <vector>

#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>
#include <spa/param/audio/type-info.h>
#include <spa/utils/result.h>

extern "C" int ss_log(const char* fmt, ...);

#ifndef PW_KEY_TARGET_OBJECT
#define PW_KEY_TARGET_OBJECT "target.object"
#endif
#ifndef PW_KEY_OBJECT_SERIAL
#define PW_KEY_OBJECT_SERIAL "object.serial"
#endif

namespace {

constexpr const char* kDefaultDeviceName = "System Default";

//==============================================================================
// dlopen shim. Only functions that are real library symbols go through this
// table — the pw_core_*/pw_registry_*/pw_node_* "methods" are header-inline
// vtable dispatches and need no loading. Calling a header-declared libpipewire
// function directly would silently reintroduce a hard link dependency, so
// everything below is invoked as api().<name>.
struct PwApi {
    void* handle = nullptr;

    void (*init)(int*, char***) = nullptr;
    const char* (*get_library_version)() = nullptr;

    pw_thread_loop* (*thread_loop_new)(const char*, const spa_dict*) = nullptr;
    void (*thread_loop_destroy)(pw_thread_loop*) = nullptr;
    int (*thread_loop_start)(pw_thread_loop*) = nullptr;
    void (*thread_loop_stop)(pw_thread_loop*) = nullptr;
    void (*thread_loop_lock)(pw_thread_loop*) = nullptr;
    void (*thread_loop_unlock)(pw_thread_loop*) = nullptr;
    int (*thread_loop_timed_wait)(pw_thread_loop*, int) = nullptr;
    void (*thread_loop_signal)(pw_thread_loop*, bool) = nullptr;
    pw_loop* (*thread_loop_get_loop)(pw_thread_loop*) = nullptr;

    pw_context* (*context_new)(pw_loop*, pw_properties*, size_t) = nullptr;
    void (*context_destroy)(pw_context*) = nullptr;
    pw_core* (*context_connect)(pw_context*, pw_properties*, size_t) = nullptr;
    int (*core_disconnect)(pw_core*) = nullptr;
    void (*proxy_destroy)(pw_proxy*) = nullptr;

    pw_properties* (*properties_new)(const char*, ...) = nullptr;
    int (*properties_set)(pw_properties*, const char*, const char*) = nullptr;
    void (*properties_free)(pw_properties*) = nullptr;

    pw_stream* (*stream_new)(pw_core*, const char*, pw_properties*) = nullptr;
    void (*stream_destroy)(pw_stream*) = nullptr;
    void (*stream_add_listener)(pw_stream*, spa_hook*, const pw_stream_events*, void*) = nullptr;
    int (*stream_connect)(pw_stream*, spa_direction, uint32_t, pw_stream_flags,
                          const spa_pod**, uint32_t) = nullptr;
    int (*stream_disconnect)(pw_stream*) = nullptr;
    pw_stream_state (*stream_get_state)(pw_stream*, const char**) = nullptr;
    pw_buffer* (*stream_dequeue_buffer)(pw_stream*) = nullptr;
    int (*stream_queue_buffer)(pw_stream*, pw_buffer*) = nullptr;

    bool load() {
        handle = dlopen("libpipewire-0.3.so.0", RTLD_NOW | RTLD_LOCAL);
        if (handle == nullptr)
            handle = dlopen("libpipewire-0.3.so", RTLD_NOW | RTLD_LOCAL);
        if (handle == nullptr)
            return false;

        bool ok = true;
        auto grab = [&](auto& fn, const char* name) {
            fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(dlsym(handle, name));
            if (fn == nullptr) ok = false;
        };
        grab(init,                  "pw_init");
        grab(get_library_version,   "pw_get_library_version");
        grab(thread_loop_new,       "pw_thread_loop_new");
        grab(thread_loop_destroy,   "pw_thread_loop_destroy");
        grab(thread_loop_start,     "pw_thread_loop_start");
        grab(thread_loop_stop,      "pw_thread_loop_stop");
        grab(thread_loop_lock,      "pw_thread_loop_lock");
        grab(thread_loop_unlock,    "pw_thread_loop_unlock");
        grab(thread_loop_timed_wait,"pw_thread_loop_timed_wait");
        grab(thread_loop_signal,    "pw_thread_loop_signal");
        grab(thread_loop_get_loop,  "pw_thread_loop_get_loop");
        grab(context_new,           "pw_context_new");
        grab(context_destroy,       "pw_context_destroy");
        grab(context_connect,       "pw_context_connect");
        grab(core_disconnect,       "pw_core_disconnect");
        grab(proxy_destroy,         "pw_proxy_destroy");
        grab(properties_new,        "pw_properties_new");
        grab(properties_set,        "pw_properties_set");
        grab(properties_free,       "pw_properties_free");
        grab(stream_new,            "pw_stream_new");
        grab(stream_destroy,        "pw_stream_destroy");
        grab(stream_add_listener,   "pw_stream_add_listener");
        grab(stream_connect,        "pw_stream_connect");
        grab(stream_disconnect,     "pw_stream_disconnect");
        grab(stream_get_state,      "pw_stream_get_state");
        grab(stream_dequeue_buffer, "pw_stream_dequeue_buffer");
        grab(stream_queue_buffer,   "pw_stream_queue_buffer");

        if (!ok) { dlclose(handle); handle = nullptr; }
        return ok;
    }
};

// Maps a port's "audio.channel" label ("FL", "FR", "AUX0", ...) to its SPA
// channel id so the stream format can request the sink's own channel layout
// and get passthrough mapping instead of remixing.
uint32_t channelIdFromLabel(const std::string& label) {
    if (label.empty())
        return SPA_AUDIO_CHANNEL_UNKNOWN;
    for (const spa_type_info* t = spa_type_audio_channel; t != nullptr && t->name != nullptr; ++t) {
        const char* colon = strrchr(t->name, ':');
        if (colon != nullptr && label == (colon + 1))
            return t->type;
    }
    return SPA_AUDIO_CHANNEL_UNKNOWN;
}

// A sink or source node as shown to JUCE. `serial.empty()` marks the
// "System Default" sentinel: its streams connect with no target so the
// daemon routes them to the current default and re-routes them when the
// user changes it (e.g. via the desktop volume applet).
struct PwNodeInfo {
    uint32_t id = PW_ID_ANY;
    std::string serial;
    std::string nodeName;
    std::string description;
    bool isSink = false;
    std::vector<std::string> channelLabels;

    int channels() const { return (int) channelLabels.size(); }
};

//==============================================================================
// Process-lifetime daemon connection + registry mirror. Never torn down: the
// loop thread and dlopen'd library outlive every AudioDeviceManager the engine
// creates (it rebuilds managers on cold recovery), and skipping shutdown
// avoids exit-order races against PipeWire's own threads — the same reason
// JUCE never dlcloses libjack.
class PipeWireSystem {
public:
    static PipeWireSystem& instance() {
        static PipeWireSystem* s = new PipeWireSystem();
        return *s;
    }

    PwApi api;

    bool libLoaded() const { return api.handle != nullptr; }
    bool connected() const { return mCore != nullptr; }
    pw_core* core() const { return mCore; }
    pw_thread_loop* loop() const { return mLoop; }

    void lock()   { api.thread_loop_lock(mLoop); }
    void unlock() { api.thread_loop_unlock(mLoop); }

    // Loads the library and starts the loop once; retries the daemon
    // connection on every call so a daemon started after boot is picked up
    // by the next device scan.
    bool ensureConnected() {
        if (!libLoaded())
            return false;
        std::lock_guard<std::mutex> g(mConnectMutex);
        if (mCore != nullptr)
            return true;

        if (mLoop == nullptr) {
            api.init(nullptr, nullptr);
            mLoop = api.thread_loop_new("supersonic-pw", nullptr);
            if (mLoop == nullptr || api.thread_loop_start(mLoop) != 0) {
                ss_log("PipeWire: failed to start thread loop");
                if (mLoop != nullptr) { api.thread_loop_destroy(mLoop); mLoop = nullptr; }
                return false;
            }
        }

        lock();
        if (mContext == nullptr)
            mContext = api.context_new(api.thread_loop_get_loop(mLoop), nullptr, 0);
        if (mContext != nullptr)
            mCore = api.context_connect(mContext, nullptr, 0);

        if (mCore != nullptr) {
            static const pw_core_events coreEvents = [] {
                pw_core_events e{};
                e.version = PW_VERSION_CORE_EVENTS;
                e.done = [](void* data, uint32_t id, int seq) {
                    auto* self = static_cast<PipeWireSystem*>(data);
                    if (id == PW_ID_CORE) {
                        self->mDoneSeq.store(seq, std::memory_order_release);
                        self->api.thread_loop_signal(self->mLoop, false);
                    }
                };
                e.error = [](void*, uint32_t id, int, int res, const char* message) {
                    ss_log("PipeWire core error: id=%u res=%d (%s)",
                           id, res, message != nullptr ? message : "");
                };
                return e;
            }();
            pw_core_add_listener(mCore, &mCoreHook, &coreEvents, this);

            mRegistry = pw_core_get_registry(mCore, PW_VERSION_REGISTRY, 0);
            static const pw_registry_events registryEvents = [] {
                pw_registry_events e{};
                e.version = PW_VERSION_REGISTRY_EVENTS;
                e.global = &PipeWireSystem::onGlobal;
                e.global_remove = &PipeWireSystem::onGlobalRemove;
                return e;
            }();
            pw_registry_add_listener(mRegistry, &mRegistryHook, &registryEvents, this);
            ss_log("PipeWire connected (libpipewire %s)", api.get_library_version());
        }
        unlock();
        return mCore != nullptr;
    }

    // Server roundtrip: returns once every registry event that was in flight
    // when it was called has been delivered, so a scan sees a complete graph.
    bool roundtrip(int timeoutSecs) {
        if (mCore == nullptr)
            return false;
        lock();
        const int seq = pw_core_sync(mCore, PW_ID_CORE, 0);
        bool done = false;
        for (int i = 0; i < timeoutSecs; ++i) {
            if (mDoneSeq.load(std::memory_order_acquire) == seq) { done = true; break; }
            if (api.thread_loop_timed_wait(mLoop, 1) != 0
                && mDoneSeq.load(std::memory_order_acquire) == seq) { done = true; break; }
        }
        if (!done)
            done = mDoneSeq.load(std::memory_order_acquire) == seq;
        unlock();
        return done;
    }

    std::vector<PwNodeInfo> snapshotNodes() {
        std::vector<PwNodeInfo> out;
        lock();
        for (const auto& n : mNodes) {
            PwNodeInfo info = n;
            // A sink consumes on its input ports, a source produces on its
            // output ports; monitor ports are the sink's loopback taps, not
            // playback channels.
            struct Slot { int index; std::string label; };
            std::vector<Slot> slots;
            for (const auto& p : mPorts) {
                if (p.nodeId != n.id || p.monitor || p.isInput != n.isSink)
                    continue;
                slots.push_back({ p.portIndex, p.channel });
            }
            std::sort(slots.begin(), slots.end(),
                      [](const Slot& a, const Slot& b) { return a.index < b.index; });
            for (auto& s : slots)
                info.channelLabels.push_back(std::move(s.label));
            out.push_back(std::move(info));
        }
        unlock();
        return out;
    }

    // Device-change fan-out to live AudioIODeviceType instances (the engine
    // rebuilds its AudioDeviceManager — and therefore our type — on cold
    // recovery, so sinks register and unregister dynamically).
    void addChangeSink(void* owner, std::function<void()> fn) {
        std::lock_guard<std::mutex> g(mSinkMutex);
        mSinks[owner] = std::move(fn);
    }
    void removeChangeSink(void* owner) {
        std::lock_guard<std::mutex> g(mSinkMutex);
        mSinks.erase(owner);
    }

private:
    PipeWireSystem() { api.load(); }

    struct PwPort {
        uint32_t globalId = 0;
        uint32_t nodeId = 0;
        bool isInput = false;
        bool monitor = false;
        int portIndex = 0;
        std::string channel;
    };

    static const char* dictGet(const spa_dict* props, const char* key) {
        return props != nullptr ? spa_dict_lookup(props, key) : nullptr;
    }

    static void onGlobal(void* data, uint32_t id, uint32_t /*permissions*/,
                         const char* type, uint32_t /*version*/, const spa_dict* props) {
        auto* self = static_cast<PipeWireSystem*>(data);

        if (type != nullptr && strcmp(type, PW_TYPE_INTERFACE_Node) == 0) {
            const char* mediaClass = dictGet(props, PW_KEY_MEDIA_CLASS);
            if (mediaClass == nullptr)
                return;
            const bool isSink   = strcmp(mediaClass, "Audio/Sink") == 0;
            const bool isSource = strcmp(mediaClass, "Audio/Source") == 0;
            if (!isSink && !isSource)
                return;

            PwNodeInfo n;
            n.id = id;
            n.isSink = isSink;
            if (const char* s = dictGet(props, PW_KEY_OBJECT_SERIAL)) n.serial = s;
            if (const char* s = dictGet(props, PW_KEY_NODE_NAME))     n.nodeName = s;
            const char* desc = dictGet(props, PW_KEY_NODE_DESCRIPTION);
            if (desc == nullptr) desc = dictGet(props, PW_KEY_NODE_NICK);
            n.description = desc != nullptr ? desc : n.nodeName;
            if (n.description.empty() || n.serial.empty())
                return;
            self->mNodes.push_back(std::move(n));
            self->notifyChanged();
            return;
        }

        if (type != nullptr && strcmp(type, PW_TYPE_INTERFACE_Port) == 0) {
            const char* nodeIdStr = dictGet(props, PW_KEY_NODE_ID);
            const char* dir       = dictGet(props, PW_KEY_PORT_DIRECTION);
            if (nodeIdStr == nullptr || dir == nullptr)
                return;
            PwPort p;
            p.globalId = id;
            p.nodeId = (uint32_t) strtoul(nodeIdStr, nullptr, 10);
            p.isInput = strcmp(dir, "in") == 0;
            if (const char* m = dictGet(props, PW_KEY_PORT_MONITOR))
                p.monitor = strcmp(m, "true") == 0;
            if (const char* idx = dictGet(props, PW_KEY_PORT_ID))
                p.portIndex = (int) strtol(idx, nullptr, 10);
            if (const char* ch = dictGet(props, PW_KEY_AUDIO_CHANNEL))
                p.channel = ch;

            // Only ports of tracked device nodes matter — this also keeps our
            // own stream nodes from triggering device-change notifications.
            for (const auto& n : self->mNodes) {
                if (n.id == p.nodeId) {
                    self->mPorts.push_back(std::move(p));
                    self->notifyChanged();
                    return;
                }
            }
        }
    }

    static void onGlobalRemove(void* data, uint32_t id) {
        auto* self = static_cast<PipeWireSystem*>(data);
        bool changed = false;
        for (auto it = self->mNodes.begin(); it != self->mNodes.end();) {
            if (it->id == id) { it = self->mNodes.erase(it); changed = true; }
            else ++it;
        }
        for (auto it = self->mPorts.begin(); it != self->mPorts.end();) {
            if (it->globalId == id) { it = self->mPorts.erase(it); changed = true; }
            else ++it;
        }
        if (changed)
            self->notifyChanged();
    }

    void notifyChanged() {
        std::lock_guard<std::mutex> g(mSinkMutex);
        for (auto& [owner, fn] : mSinks)
            fn();
    }

    std::mutex mConnectMutex;
    pw_thread_loop* mLoop = nullptr;
    pw_context* mContext = nullptr;
    pw_core* mCore = nullptr;
    pw_registry* mRegistry = nullptr;
    spa_hook mCoreHook{};
    spa_hook mRegistryHook{};
    std::atomic<int> mDoneSeq { -1 };

    // Guarded by the thread-loop lock (mutated only in registry events).
    std::vector<PwNodeInfo> mNodes;
    std::vector<PwPort> mPorts;

    std::mutex mSinkMutex;
    std::map<void*, std::function<void()>> mSinks;
};

//==============================================================================
// SPSC ring carrying planar capture audio from the capture stream's process
// callback to the playback stream's (both on PipeWire data threads, commonly
// the same one). Capacity bounds added input latency; on overflow the newest
// frames are dropped so the reader's view stays contiguous.
class CaptureRing {
public:
    void init(int channels, uint32_t capacityFramesPow2) {
        mChannels = channels;
        mCap = capacityFramesPow2;
        mMask = capacityFramesPow2 - 1;
        mBuf.assign((size_t) channels * capacityFramesPow2, 0.0f);
        mW.store(0, std::memory_order_relaxed);
        mR.store(0, std::memory_order_relaxed);
    }

    void write(const float* const* planes, int channels, uint32_t frames) {
        const uint64_t w = mW.load(std::memory_order_relaxed);
        const uint64_t r = mR.load(std::memory_order_acquire);
        const uint32_t space = mCap - (uint32_t) (w - r);
        if (frames > space)
            frames = space;
        if (frames == 0)
            return;
        const uint32_t idx = (uint32_t) w & mMask;
        const uint32_t first = std::min(frames, mCap - idx);
        const int nch = std::min(channels, mChannels);
        for (int ch = 0; ch < nch; ++ch) {
            float* base = mBuf.data() + (size_t) ch * mCap;
            memcpy(base + idx, planes[ch], first * sizeof(float));
            if (first < frames)
                memcpy(base, planes[ch] + first, (frames - first) * sizeof(float));
        }
        mW.store(w + frames, std::memory_order_release);
    }

    // Copies up to `frames` frames into dest planes, zero-padding any
    // shortfall so the consumer always gets full blocks.
    void read(float* const* dest, int channels, uint32_t frames) {
        const uint64_t w = mW.load(std::memory_order_acquire);
        const uint64_t r = mR.load(std::memory_order_relaxed);
        const uint32_t avail = (uint32_t) (w - r);
        const uint32_t take = std::min(frames, avail);
        const uint32_t idx = (uint32_t) r & mMask;
        const uint32_t first = std::min(take, mCap - idx);
        for (int ch = 0; ch < channels; ++ch) {
            const float* base = mBuf.data() + (size_t) std::min(ch, mChannels - 1) * mCap;
            if (ch < mChannels) {
                memcpy(dest[ch], base + idx, first * sizeof(float));
                if (first < take)
                    memcpy(dest[ch] + first, base, (take - first) * sizeof(float));
            }
            if (take < frames || ch >= mChannels)
                memset(dest[ch] + (ch < mChannels ? take : 0), 0,
                       (frames - (ch < mChannels ? take : 0)) * sizeof(float));
        }
        mR.store(r + take, std::memory_order_release);
    }

private:
    std::vector<float> mBuf;
    int mChannels = 0;
    uint32_t mCap = 0, mMask = 0;
    std::atomic<uint64_t> mW { 0 }, mR { 0 };
};

//==============================================================================
class PipeWireAudioIODevice final : public juce::AudioIODevice {
public:
    PipeWireAudioIODevice(const juce::String& outName, const juce::String& inName,
                          PwNodeInfo outInfo, PwNodeInfo inInfo)
        : juce::AudioIODevice(outName.isNotEmpty() ? outName : inName, "PipeWire"),
          outputName(outName), inputName(inName),
          mOutInfo(std::move(outInfo)), mInInfo(std::move(inInfo)) {
        // Nodes whose ports weren't visible at scan time (and the default
        // sentinel) fall back to stereo — but only on sides this device was
        // actually created with, otherwise an output-only probe would report
        // phantom input channels.
        if (outputName.isNotEmpty() && mOutInfo.channelLabels.empty())
            mOutInfo.channelLabels = { "FL", "FR" };
        if (inputName.isNotEmpty() && mInInfo.channelLabels.empty())
            mInInfo.channelLabels = { "FL", "FR" };
    }

    ~PipeWireAudioIODevice() override { close(); }

    juce::StringArray getOutputChannelNames() override { return channelNames(mOutInfo, "Out"); }
    juce::StringArray getInputChannelNames() override  { return channelNames(mInInfo, "In"); }

    juce::Array<double> getAvailableSampleRates() override {
        // The daemon resamples any stream rate to the graph rate, so all the
        // engine's standard rates are genuinely available.
        return { 44100.0, 48000.0, 88200.0, 96000.0, 176400.0, 192000.0 };
    }

    juce::Array<int> getAvailableBufferSizes() override {
        return { 32, 64, 128, 256, 512, 1024, 2048, 4096 };
    }

    int getDefaultBufferSize() override { return 256; }

    juce::String open(const juce::BigInteger& inputChannels,
                      const juce::BigInteger& outputChannels,
                      double sampleRate, int bufferSizeSamples) override {
        close();
        auto& sys = PipeWireSystem::instance();
        if (!sys.ensureConnected()) {
            mLastError = "PipeWire daemon is not reachable";
            return mLastError;
        }

        mRate = sampleRate > 0 ? (int) sampleRate : 48000;
        mBufFrames = bufferSizeSamples > 0 ? bufferSizeSamples : getDefaultBufferSize();

        mNumOut = (outputName.isNotEmpty() && !outputChannels.isZero())
                      ? juce::jlimit(1, mOutInfo.channels(), outputChannels.getHighestBit() + 1)
                      : 0;
        mNumIn = (inputName.isNotEmpty() && !inputChannels.isZero())
                     ? juce::jlimit(1, mInInfo.channels(), inputChannels.getHighestBit() + 1)
                     : 0;
        if (mNumOut == 0 && mNumIn == 0) {
            mLastError = "no channels requested";
            return mLastError;
        }

        mOutPtrs.assign((size_t) std::max(mNumOut, 1), nullptr);
        if (mNumIn > 0) {
            mRing.init(mNumIn, 32768);
            mInScratch.assign((size_t) mNumIn * kMaxQuantum, 0.0f);
            mInScratchPtrs.resize((size_t) mNumIn);
            for (int ch = 0; ch < mNumIn; ++ch)
                mInScratchPtrs[(size_t) ch] = mInScratch.data() + (size_t) ch * kMaxQuantum;
        }

        juce::String err;
        sys.lock();
        if (mNumOut > 0)
            mOutStream = makeStream(true, mOutInfo, mNumOut, mOutHook, err);
        if (err.isEmpty() && mNumIn > 0)
            mInStream = makeStream(false, mInInfo, mNumIn, mInHook, err);
        sys.unlock();

        if (err.isEmpty())
            err = waitForStreams();

        if (err.isNotEmpty()) {
            close();
            mLastError = err;
            return err;
        }

        mActiveOut.clear();
        mActiveOut.setRange(0, mNumOut, true);
        mActiveIn.clear();
        mActiveIn.setRange(0, mNumIn, true);
        mIsOpen = true;
        mLastError.clear();
        return {};
    }

    void close() override {
        stop();
        auto& sys = PipeWireSystem::instance();
        if (sys.connected() && (mOutStream != nullptr || mInStream != nullptr)) {
            sys.lock();
            for (pw_stream** s : { &mOutStream, &mInStream }) {
                if (*s != nullptr) {
                    sys.api.stream_disconnect(*s);
                    sys.api.stream_destroy(*s);   // also unhooks our listeners
                    *s = nullptr;
                }
            }
            sys.unlock();
        }
        mOutStream = mInStream = nullptr;
        mOutHook = spa_hook{};
        mInHook = spa_hook{};
        mIsOpen = false;
    }

    void start(juce::AudioIODeviceCallback* newCallback) override {
        if (mIsOpen && newCallback != mCallback) {
            if (newCallback != nullptr)
                newCallback->audioDeviceAboutToStart(this);
            juce::AudioIODeviceCallback* old = mCallback;
            {
                const juce::ScopedLock sl(mCallbackLock);
                mCallback = newCallback;
            }
            if (old != nullptr)
                old->audioDeviceStopped();
        }
    }

    void stop() override { start(nullptr); }

    bool isOpen() override    { return mIsOpen; }
    bool isPlaying() override { return mCallback != nullptr; }
    juce::String getLastError() override { return mLastError; }

    int getCurrentBufferSizeSamples() override { return mBufFrames; }
    double getCurrentSampleRate() override     { return mRate; }
    int getCurrentBitDepth() override          { return 32; }

    juce::BigInteger getActiveOutputChannels() const override { return mActiveOut; }
    juce::BigInteger getActiveInputChannels() const override  { return mActiveIn; }

    // One quantum each way is what the graph adds between our node and the
    // device node; device-internal latency is not visible through the stream
    // API, so this is a floor rather than an exact figure.
    int getOutputLatencyInSamples() override { return mBufFrames; }
    int getInputLatencyInSamples() override  { return mBufFrames; }

    juce::String outputName, inputName;

private:
    // PipeWire quantum ceiling (default max in the daemon's config); sizes
    // the capture scratch planes and caps the per-cycle frame count so
    // process() never allocates and never outgrows them.
    static constexpr uint32_t kMaxQuantum = 8192;

    juce::StringArray channelNames(const PwNodeInfo& info, const char* prefix) const {
        juce::StringArray names;
        int i = 1;
        for (const auto& label : info.channelLabels)
            names.add(label.empty() || label == "UNK"
                          ? juce::String(prefix) + " " + juce::String(i++)
                          : juce::String(label));
        return names;
    }

    static void fillPositions(spa_audio_info_raw& fmt, const PwNodeInfo& info, int channels) {
        for (int i = 0; i < channels && i < SPA_AUDIO_MAX_CHANNELS; ++i) {
            uint32_t id = i < info.channels() ? channelIdFromLabel(info.channelLabels[(size_t) i])
                                              : SPA_AUDIO_CHANNEL_UNKNOWN;
            if (id == SPA_AUDIO_CHANNEL_UNKNOWN)
                id = std::min<uint32_t>(SPA_AUDIO_CHANNEL_AUX0 + (uint32_t) i,
                                        SPA_AUDIO_CHANNEL_AUX63);
            fmt.position[i] = id;
        }
    }

    // Caller must hold the thread-loop lock.
    pw_stream* makeStream(bool playback, const PwNodeInfo& node, int channels,
                          spa_hook& hook, juce::String& err) {
        auto& sys = PipeWireSystem::instance();
        auto& A = sys.api;

        pw_properties* props = A.properties_new(
            PW_KEY_MEDIA_TYPE, "Audio",
            PW_KEY_MEDIA_CATEGORY, playback ? "Playback" : "Capture",
            PW_KEY_MEDIA_ROLE, "Production",
            PW_KEY_APP_NAME, "SuperSonic",
            PW_KEY_NODE_NAME, playback ? "SuperSonic" : "SuperSonic Input",
            // Keep the graph driving us while idle: the engine's clock,
            // worker wakeups and the recovery watchdog are all fed off the
            // process callback, so a suspended stream reads as a dead device.
            PW_KEY_NODE_ALWAYS_PROCESS, "true",
            nullptr);
        char tmp[64];
        snprintf(tmp, sizeof(tmp), "%d/%d", mBufFrames, mRate);
        A.properties_set(props, PW_KEY_NODE_LATENCY, tmp);
        snprintf(tmp, sizeof(tmp), "1/%d", mRate);
        A.properties_set(props, PW_KEY_NODE_RATE, tmp);
        if (!node.serial.empty())
            A.properties_set(props, PW_KEY_TARGET_OBJECT, node.serial.c_str());

        pw_stream* s = A.stream_new(sys.core(), playback ? "SuperSonic Out" : "SuperSonic In", props);
        if (s == nullptr) {
            err = "pw_stream_new failed";
            return nullptr;
        }

        static const pw_stream_events outEvents = makeStreamEvents(true);
        static const pw_stream_events inEvents = makeStreamEvents(false);
        A.stream_add_listener(s, &hook, playback ? &outEvents : &inEvents, this);

        spa_audio_info_raw fmt{};
        fmt.format = SPA_AUDIO_FORMAT_F32P;
        fmt.rate = (uint32_t) mRate;
        fmt.channels = (uint32_t) channels;
        fillPositions(fmt, node, channels);

        uint8_t podBuf[1024];
        spa_pod_builder b = SPA_POD_BUILDER_INIT(podBuf, sizeof(podBuf));
        const spa_pod* params[1] = { spa_format_audio_raw_build(&b, SPA_PARAM_EnumFormat, &fmt) };

        const int res = A.stream_connect(
            s, playback ? PW_DIRECTION_OUTPUT : PW_DIRECTION_INPUT, PW_ID_ANY,
            (pw_stream_flags) (PW_STREAM_FLAG_AUTOCONNECT
                               | PW_STREAM_FLAG_MAP_BUFFERS
                               | PW_STREAM_FLAG_RT_PROCESS),
            params, 1);
        if (res < 0)
            err = "pw_stream_connect failed: " + juce::String(spa_strerror(res));
        return s;
    }

    static pw_stream_events makeStreamEvents(bool playback) {
        pw_stream_events e{};
        e.version = PW_VERSION_STREAM_EVENTS;
        e.state_changed = [](void* /*data*/, pw_stream_state /*old*/, pw_stream_state state,
                             const char* error) {
            if (state == PW_STREAM_STATE_ERROR)
                ss_log("PipeWire stream error: %s", error != nullptr ? error : "unknown");
            auto& sys = PipeWireSystem::instance();
            sys.api.thread_loop_signal(sys.loop(), false);
        };
        if (playback)
            e.process = [](void* data) { static_cast<PipeWireAudioIODevice*>(data)->processPlayback(); };
        else
            e.process = [](void* data) { static_cast<PipeWireAudioIODevice*>(data)->processCapture(); };
        return e;
    }

    juce::String waitForStreams() {
        auto& sys = PipeWireSystem::instance();
        auto& A = sys.api;
        juce::String err;
        sys.lock();
        for (int elapsed = 0; elapsed < 5; ++elapsed) {
            const char* streamError = nullptr;
            bool ready = true;
            for (pw_stream* s : { mOutStream, mInStream }) {
                if (s == nullptr)
                    continue;
                const pw_stream_state st = A.stream_get_state(s, &streamError);
                if (st == PW_STREAM_STATE_ERROR) {
                    err = "PipeWire stream failed: "
                          + juce::String(streamError != nullptr ? streamError : "unknown");
                    ready = false;
                    break;
                }
                if (st != PW_STREAM_STATE_PAUSED && st != PW_STREAM_STATE_STREAMING)
                    ready = false;
            }
            if (ready || err.isNotEmpty())
                break;
            A.thread_loop_timed_wait(sys.loop(), 1);
        }
        sys.unlock();
        return err;
    }

    void processPlayback() {
        auto& A = PipeWireSystem::instance().api;
        pw_buffer* b = A.stream_dequeue_buffer(mOutStream);
        if (b == nullptr)
            return;
        spa_buffer* sb = b->buffer;
        const int nCh = std::min((int) sb->n_datas, mNumOut);
        if (nCh == 0) {
            A.stream_queue_buffer(mOutStream, b);
            return;
        }
        const uint32_t maxFrames = sb->datas[0].maxsize / sizeof(float);
        uint32_t n = b->requested > 0 ? std::min((uint32_t) b->requested, maxFrames) : maxFrames;
        n = std::min(n, kMaxQuantum);
        if (n == 0) {
            A.stream_queue_buffer(mOutStream, b);
            return;
        }

        for (int ch = 0; ch < nCh; ++ch)
            mOutPtrs[(size_t) ch] = (float*) sb->datas[ch].data;

        const float* const* inPtrs = nullptr;
        int nIn = 0;
        if (mNumIn > 0) {
            mRing.read(mInScratchPtrs.data(), mNumIn, n);
            inPtrs = const_cast<const float* const*>(mInScratchPtrs.data());
            nIn = mNumIn;
        }

        {
            const juce::ScopedLock sl(mCallbackLock);
            if (mCallback != nullptr)
                mCallback->audioDeviceIOCallbackWithContext(inPtrs, nIn, mOutPtrs.data(),
                                                            nCh, (int) n, {});
            else
                for (int ch = 0; ch < nCh; ++ch)
                    memset(mOutPtrs[(size_t) ch], 0, n * sizeof(float));
        }

        for (int ch = 0; ch < nCh; ++ch) {
            sb->datas[ch].chunk->offset = 0;
            sb->datas[ch].chunk->stride = sizeof(float);
            sb->datas[ch].chunk->size = n * (uint32_t) sizeof(float);
        }
        A.stream_queue_buffer(mOutStream, b);
    }

    void processCapture() {
        auto& A = PipeWireSystem::instance().api;
        pw_buffer* b = A.stream_dequeue_buffer(mInStream);
        if (b == nullptr)
            return;
        spa_buffer* sb = b->buffer;
        const int nCh = std::min((int) sb->n_datas, mNumIn);
        if (nCh > 0) {
            const float* planes[SPA_AUDIO_MAX_CHANNELS];
            uint32_t frames = UINT32_MAX;
            for (int ch = 0; ch < nCh; ++ch) {
                const auto& d = sb->datas[ch];
                const uint32_t stride = d.chunk->stride > 0 ? (uint32_t) d.chunk->stride
                                                            : (uint32_t) sizeof(float);
                const uint32_t sz = std::min(d.chunk->size, d.maxsize);
                frames = std::min(frames, sz / stride);
                planes[ch] = (const float*) ((const uint8_t*) d.data + d.chunk->offset);
            }
            if (frames > 0 && frames != UINT32_MAX) {
                if (mOutStream != nullptr) {
                    mRing.write(planes, nCh, frames);
                } else {
                    // Input-only device: the capture stream is the clock.
                    const juce::ScopedLock sl(mCallbackLock);
                    if (mCallback != nullptr)
                        mCallback->audioDeviceIOCallbackWithContext(planes, nCh, nullptr, 0,
                                                                    (int) frames, {});
                }
            }
        }
        A.stream_queue_buffer(mInStream, b);
    }

    PwNodeInfo mOutInfo, mInInfo;
    pw_stream* mOutStream = nullptr;
    pw_stream* mInStream = nullptr;
    spa_hook mOutHook{};
    spa_hook mInHook{};

    int mRate = 48000;
    int mBufFrames = 256;
    int mNumOut = 0, mNumIn = 0;
    bool mIsOpen = false;
    juce::String mLastError;
    juce::BigInteger mActiveOut, mActiveIn;

    juce::AudioIODeviceCallback* mCallback = nullptr;
    juce::CriticalSection mCallbackLock;

    std::vector<float*> mOutPtrs;
    std::vector<float> mInScratch;
    std::vector<float*> mInScratchPtrs;
    CaptureRing mRing;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PipeWireAudioIODevice)
};

//==============================================================================
class PipeWireAudioIODeviceType final : public juce::AudioIODeviceType,
                                        private juce::AsyncUpdater {
public:
    PipeWireAudioIODeviceType() : AudioIODeviceType("PipeWire") {
        PipeWireSystem::instance().addChangeSink(this, [this] { triggerAsyncUpdate(); });
    }

    ~PipeWireAudioIODeviceType() override {
        PipeWireSystem::instance().removeChangeSink(this);
        cancelPendingUpdate();
    }

    void scanForDevices() override {
        hasScanned = true;
        outputNames.clear();
        inputNames.clear();
        mOutputs.clear();
        mInputs.clear();

        auto& sys = PipeWireSystem::instance();
        if (!sys.ensureConnected())
            return;
        sys.roundtrip(2);

        auto nodes = sys.snapshotNodes();

        // The default sentinel targets nothing, so it only makes sense when
        // there is at least one real endpoint in that direction.
        auto addDefaults = [&](bool sinks) {
            for (const auto& n : nodes) {
                if (n.isSink == sinks) {
                    PwNodeInfo def;
                    def.description = kDefaultDeviceName;
                    def.isSink = sinks;
                    (sinks ? mOutputs : mInputs).push_back(def);
                    (sinks ? outputNames : inputNames).add(kDefaultDeviceName);
                    return;
                }
            }
        };
        addDefaults(true);
        addDefaults(false);

        auto add = [](std::vector<PwNodeInfo>& infos, juce::StringArray& names, PwNodeInfo n) {
            juce::String display(n.description);
            // JUCE device names must be unique within a type; match its
            // " (N)" disambiguation convention.
            int suffix = 2;
            while (names.contains(display))
                display = juce::String(n.description) + " (" + juce::String(suffix++) + ")";
            n.description = display.toStdString();
            names.add(display);
            infos.push_back(std::move(n));
        };
        for (auto& n : nodes) {
            if (n.isSink) add(mOutputs, outputNames, std::move(n));
            else          add(mInputs, inputNames, std::move(n));
        }
    }

    juce::StringArray getDeviceNames(bool wantInputNames) const override {
        jassert(hasScanned);
        return wantInputNames ? inputNames : outputNames;
    }

    int getDefaultDeviceIndex(bool /*forInput*/) const override {
        jassert(hasScanned);
        return 0;   // "System Default"
    }

    bool hasSeparateInputsAndOutputs() const override { return true; }

    int getIndexOfDevice(juce::AudioIODevice* device, bool asInput) const override {
        jassert(hasScanned);
        if (auto* d = dynamic_cast<PipeWireAudioIODevice*>(device))
            return asInput ? inputNames.indexOf(d->inputName)
                           : outputNames.indexOf(d->outputName);
        return -1;
    }

    juce::AudioIODevice* createDevice(const juce::String& outputDeviceName,
                                      const juce::String& inputDeviceName) override {
        jassert(hasScanned);
        const int outIdx = outputNames.indexOf(outputDeviceName);
        const int inIdx = inputNames.indexOf(inputDeviceName);
        if (outIdx < 0 && inIdx < 0)
            return nullptr;
        return new PipeWireAudioIODevice(
            outIdx >= 0 ? outputDeviceName : juce::String(),
            inIdx >= 0 ? inputDeviceName : juce::String(),
            outIdx >= 0 ? mOutputs[(size_t) outIdx] : PwNodeInfo{},
            inIdx >= 0 ? mInputs[(size_t) inIdx] : PwNodeInfo{});
    }

private:
    // Registry changed on the loop thread; refresh the snapshot before
    // notifying so listeners compare against the post-change device lists.
    void handleAsyncUpdate() override { scanForDevices(); callDeviceChangeListeners(); }

    juce::StringArray outputNames, inputNames;
    std::vector<PwNodeInfo> mOutputs, mInputs;
    bool hasScanned = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PipeWireAudioIODeviceType)
};

} // namespace

std::unique_ptr<juce::AudioIODeviceType> createPipeWireAudioIODeviceType() {
    // Library presence alone decides registration; daemon connection is
    // retried on every scan so a daemon started after boot still shows up.
    if (!PipeWireSystem::instance().libLoaded())
        return nullptr;
    return std::make_unique<PipeWireAudioIODeviceType>();
}

#endif // __linux__ && SUPERSONIC_PIPEWIRE
