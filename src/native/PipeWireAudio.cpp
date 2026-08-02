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
#include <pipewire/extensions/metadata.h>
#include <spa/param/audio/format-utils.h>
#include <spa/param/audio/type-info.h>
#include <spa/utils/result.h>

extern "C" int ss_log(const char* fmt, ...);
extern "C" const char* ss_app_name();

#ifndef PW_KEY_TARGET_OBJECT
#define PW_KEY_TARGET_OBJECT "target.object"
#endif
#ifndef PW_KEY_OBJECT_SERIAL
#define PW_KEY_OBJECT_SERIAL "object.serial"
#endif
#ifndef PW_KEY_NODE_MAX_LATENCY
#define PW_KEY_NODE_MAX_LATENCY "node.max-latency"
#endif

#include "HardeningPolicy.h"

namespace {

constexpr const char* kDefaultDeviceName = "System Default";

// The patchbay device: one pw_filter node with explicit graph ports, the
// same shape a JACK client has. Port count is fixed and independent of any
// sink's channel layout, and the session manager's stream remix policy
// does not apply — ports are patched (qpwgraph, pw-link, a DAW) rather
// than routed. The first pair each way is auto-linked to the default
// sink/source so the device makes sound before any patching.
constexpr const char* kPatchbayDeviceName = "Patchbay (16 ch)";
constexpr int kPatchbayChans = 16;

// PipeWire quantum ceiling (default max in the daemon's config); sizes
// scratch planes and caps the per-cycle frame count so process callbacks
// never allocate and never outgrow them.
constexpr uint32_t kMaxQuantum = 8192;

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

    pw_filter* (*filter_new)(pw_core*, const char*, pw_properties*) = nullptr;
    void (*filter_destroy)(pw_filter*) = nullptr;
    void (*filter_add_listener)(pw_filter*, spa_hook*, const pw_filter_events*, void*) = nullptr;
    int (*filter_connect)(pw_filter*, pw_filter_flags, const spa_pod**, uint32_t) = nullptr;
    int (*filter_disconnect)(pw_filter*) = nullptr;
    pw_filter_state (*filter_get_state)(pw_filter*, const char**) = nullptr;
    uint32_t (*filter_get_node_id)(pw_filter*) = nullptr;
    void* (*filter_add_port)(pw_filter*, pw_direction, pw_filter_port_flags, size_t,
                             pw_properties*, const spa_pod**, uint32_t) = nullptr;
    void* (*filter_get_dsp_buffer)(void*, uint32_t) = nullptr;

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
        grab(filter_new,            "pw_filter_new");
        grab(filter_destroy,        "pw_filter_destroy");
        grab(filter_add_listener,   "pw_filter_add_listener");
        grab(filter_connect,        "pw_filter_connect");
        grab(filter_disconnect,     "pw_filter_disconnect");
        grab(filter_get_state,      "pw_filter_get_state");
        grab(filter_get_node_id,    "pw_filter_get_node_id");
        grab(filter_add_port,       "pw_filter_add_port");
        grab(filter_get_dsp_buffer, "pw_filter_get_dsp_buffer");

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

    // Where the patchbay's auto-links should land: the default sink/source
    // per the "default" metadata, falling back to the first endpoint when
    // the metadata is absent. portIds are the linkable ports (sinks consume
    // on inputs, sources produce on non-monitor outputs) in index order.
    // Caller must hold the thread-loop lock.
    struct LinkTarget {
        uint32_t nodeId = PW_ID_ANY;
        std::vector<uint32_t> portIds;
    };

    LinkTarget defaultTarget(bool sink) const {
        LinkTarget t;
        const std::string& want = sink ? mDefaultSinkName : mDefaultSourceName;
        const PwNodeInfo* found = nullptr;
        for (const auto& n : mNodes) {
            if (n.isSink != sink)
                continue;
            if (found == nullptr)
                found = &n;                       // fallback: first endpoint
            if (!want.empty() && n.nodeName == want) { found = &n; break; }
        }
        if (found == nullptr)
            return t;
        t.nodeId = found->id;
        struct Slot { int index; uint32_t globalId; };
        std::vector<Slot> slots;
        for (const auto& p : mPorts)
            if (p.nodeId == found->id && !p.monitor && p.isInput == sink)
                slots.push_back({ p.portIndex, p.globalId });
        std::sort(slots.begin(), slots.end(),
                  [](const Slot& a, const Slot& b) { return a.index < b.index; });
        for (const auto& s : slots)
            t.portIds.push_back(s.globalId);
        return t;
    }

    // Registry global id of one of our own filter ports, found by node id,
    // direction and per-direction index. Caller must hold the loop lock.
    uint32_t portGlobalId(uint32_t nodeId, bool isInput, int index) const {
        for (const auto& p : mPorts)
            if (p.nodeId == nodeId && p.isInput == isInput && !p.monitor
                && p.portIndex == index)
                return p.globalId;
        return PW_ID_ANY;
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

            // Every port is recorded (the patchbay links by port global id,
            // including its own filter ports), but only ports of tracked
            // device nodes notify — our own stream/filter nodes appearing
            // must not read as a device change.
            const uint32_t nodeId = p.nodeId;
            self->mPorts.push_back(std::move(p));
            for (const auto& n : self->mNodes) {
                if (n.id == nodeId) {
                    self->notifyChanged();
                    return;
                }
            }
            return;
        }

        if (type != nullptr && strcmp(type, PW_TYPE_INTERFACE_Metadata) == 0) {
            // The "default" metadata carries the session manager's default
            // sink/source choices; the patchbay follows them for its
            // auto-links the way WirePlumber re-routes streams.
            const char* name = dictGet(props, PW_KEY_METADATA_NAME);
            if (name == nullptr || strcmp(name, "default") != 0 || self->mMetadata != nullptr)
                return;
            self->mMetadata = (pw_metadata*) pw_registry_bind(
                self->mRegistry, id, type, PW_VERSION_METADATA, 0);
            if (self->mMetadata == nullptr)
                return;
            self->mMetadataId = id;
            static const pw_metadata_events metadataEvents = [] {
                pw_metadata_events e{};
                e.version = PW_VERSION_METADATA_EVENTS;
                e.property = [](void* data, uint32_t /*subject*/, const char* key,
                                const char* /*type*/, const char* value) -> int {
                    auto* s = static_cast<PipeWireSystem*>(data);
                    if (key == nullptr)
                        return 0;
                    // The *configured* defaults are what the user last chose
                    // and can outlive the hardware (stale session-manager
                    // state after a sound-card swap); tracked separately for
                    // the ghost diagnostic, they never drive routing.
                    const bool cfgSink = strcmp(key, "default.configured.audio.sink") == 0;
                    const bool cfgSource = strcmp(key, "default.configured.audio.source") == 0;
                    if (cfgSink || cfgSource) {
                        (cfgSink ? s->mConfiguredSinkName : s->mConfiguredSourceName) =
                            parseNameFromJson(value);
                        s->warnIfConfiguredDefaultMissing();
                        return 0;
                    }
                    const bool sink = strcmp(key, "default.audio.sink") == 0;
                    const bool source = strcmp(key, "default.audio.source") == 0;
                    if (!sink && !source)
                        return 0;
                    std::string& slot = sink ? s->mDefaultSinkName : s->mDefaultSourceName;
                    std::string parsed = parseNameFromJson(value);
                    if (slot != parsed) {
                        slot = std::move(parsed);
                        s->notifyChanged();
                    }
                    return 0;
                };
                return e;
            }();
            pw_metadata_add_listener(self->mMetadata, &self->mMetadataHook,
                                     &metadataEvents, self);
        }
    }

    // Values look like {"name":"alsa_output.pci-....analog-stereo"}.
    static std::string parseNameFromJson(const char* value) {
        if (value == nullptr)
            return {};
        const char* key = strstr(value, "\"name\"");
        if (key == nullptr)
            return {};
        const char* open = strchr(key + 6, '"');
        if (open == nullptr)
            return {};
        const char* close = strchr(open + 1, '"');
        if (close == nullptr)
            return {};
        return std::string(open + 1, (size_t) (close - open - 1));
    }

    static void onGlobalRemove(void* data, uint32_t id) {
        auto* self = static_cast<PipeWireSystem*>(data);
        if (self->mMetadata != nullptr && id == self->mMetadataId) {
            spa_hook_remove(&self->mMetadataHook);
            self->api.proxy_destroy((pw_proxy*) self->mMetadata);
            self->mMetadata = nullptr;
            self->mMetadataId = PW_ID_ANY;
        }
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
        warnIfConfiguredDefaultMissing();
        std::lock_guard<std::mutex> g(mSinkMutex);
        for (auto& [owner, fn] : mSinks)
            fn();
    }

    // Surfaces the ghost-default condition (#3553): the session manager's
    // configured default names hardware that is no longer in the graph,
    // which silently changes how streams negotiate. Edge-triggered so a
    // persistent ghost logs once, and re-arms if it is fixed and recurs.
    // Caller must hold the thread-loop lock.
    void warnIfConfiguredDefaultMissing() {
        auto check = [&](const std::string& want, bool sink, bool& warned) {
            std::vector<std::string> present;
            for (const auto& n : mNodes)
                if (n.isSink == sink)
                    present.push_back(n.nodeName);
            const bool missing = hardening::defaultNodeMissing(want, present);
            if (missing && !warned)
                ss_log("PipeWire: configured default %s '%s' is not present in the "
                       "graph — audio may route to a fallback device (stale "
                       "session-manager state from a removed sound card?)",
                       sink ? "output" : "input", want.c_str());
            warned = missing;
        };
        check(mConfiguredSinkName, true, mWarnedGhostSink);
        check(mConfiguredSourceName, false, mWarnedGhostSource);
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
    pw_metadata* mMetadata = nullptr;
    uint32_t mMetadataId = PW_ID_ANY;
    spa_hook mMetadataHook{};
    std::string mDefaultSinkName, mDefaultSourceName;
    std::string mConfiguredSinkName, mConfiguredSourceName;
    bool mWarnedGhostSink = false, mWarnedGhostSource = false;

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

        // pw_properties copies its values, so the temporaries are safe.
        const juce::String appName(ss_app_name());
        const juce::String inputNodeName = appName + " Input";
        pw_properties* props = A.properties_new(
            PW_KEY_MEDIA_TYPE, "Audio",
            PW_KEY_MEDIA_CATEGORY, playback ? "Playback" : "Capture",
            PW_KEY_MEDIA_ROLE, "Production",
            PW_KEY_APP_NAME, appName.toRawUTF8(),
            PW_KEY_NODE_NAME, playback ? appName.toRawUTF8() : inputNodeName.toRawUTF8(),
            // Keep the graph driving us while idle: the engine's clock,
            // worker wakeups and the recovery watchdog are all fed off the
            // process callback, so a suspended stream reads as a dead device.
            PW_KEY_NODE_ALWAYS_PROCESS, "true",
            nullptr);
        char tmp[64];
        snprintf(tmp, sizeof(tmp), "%d/%d", mBufFrames, mRate);
        A.properties_set(props, PW_KEY_NODE_LATENCY, tmp);
        // Cap the quantum the graph may schedule us at: without a ceiling
        // the session manager can run the stream at its global maximum
        // (8192 frames = 170ms at 48k — sonic-pi #3553), far beyond
        // anything the engine asked for.
        snprintf(tmp, sizeof(tmp), "%d/%d", std::max(mBufFrames * 2, 2048), mRate);
        A.properties_set(props, PW_KEY_NODE_MAX_LATENCY, tmp);
        snprintf(tmp, sizeof(tmp), "1/%d", mRate);
        A.properties_set(props, PW_KEY_NODE_RATE, tmp);
        if (!node.serial.empty())
            A.properties_set(props, PW_KEY_TARGET_OBJECT, node.serial.c_str());

        const juce::String streamName = appName + (playback ? " Out" : " In");
        pw_stream* s = A.stream_new(sys.core(), streamName.toRawUTF8(), props);
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
#if PW_CHECK_VERSION(0, 3, 49)
        // pw_buffer::requested arrived in 0.3.49; older headers (Ubuntu 22.04
        // ships 0.3.48) fill the whole buffer, as pre-0.3.49 apps always did.
        uint32_t n = b->requested > 0 ? std::min((uint32_t) b->requested, maxFrames) : maxFrames;
#else
        uint32_t n = maxFrames;
#endif
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
// The patchbay device: a single pw_filter node carrying out_1..out_16 and
// in_1..in_16 as explicit graph ports. Filters bypass the stream adapter
// entirely, so there is no resampling (the node runs at the graph rate,
// reported back from open()) and no session-manager channel remixing —
// which is the point. Auto-links tie the first pair each way to the
// default sink/source, following the "default" metadata the way
// WirePlumber re-routes streams; everything else is left to the user's
// patchbay. Capture and playback share one process callback, so input is
// sample-synchronous with output and needs no ring.
class PatchbayAudioIODevice final : public juce::AudioIODevice {
public:
    PatchbayAudioIODevice()
        : juce::AudioIODevice(kPatchbayDeviceName, "PipeWire") {}

    ~PatchbayAudioIODevice() override { close(); }

    juce::StringArray getOutputChannelNames() override { return portNames("out_"); }
    juce::StringArray getInputChannelNames() override  { return portNames("in_"); }

    // The graph decides the real rate; open() reports it back, and the
    // engine adopts actual-over-requested the same way it does for a
    // device whose hardware rate differs from the request.
    juce::Array<double> getAvailableSampleRates() override {
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
        auto& A = sys.api;
        if (!sys.ensureConnected()) {
            mLastError = "PipeWire daemon is not reachable";
            return mLastError;
        }

        mClosing.store(false, std::memory_order_relaxed);
        mRate = sampleRate > 0 ? (int) sampleRate : 48000;
        mBufFrames = bufferSizeSamples > 0 ? bufferSizeSamples : getDefaultBufferSize();
        mNumOut = outputChannels.isZero() ? 0
                    : juce::jlimit(1, kPatchbayChans, outputChannels.getHighestBit() + 1);
        mNumIn = inputChannels.isZero() ? 0
                    : juce::jlimit(1, kPatchbayChans, inputChannels.getHighestBit() + 1);
        if (mNumOut == 0 && mNumIn == 0) {
            mLastError = "no channels requested";
            return mLastError;
        }

        mOutPorts.assign((size_t) mNumOut, nullptr);
        mInPorts.assign((size_t) mNumIn, nullptr);
        mOutPtrs.assign((size_t) std::max(mNumOut, 1), nullptr);
        mInPtrs.assign((size_t) std::max(mNumIn, 1), nullptr);
        mZeroPlane.assign(kMaxQuantum, 0.0f);
        mTrashPlane.assign(kMaxQuantum, 0.0f);
        mObservedRate.store(0, std::memory_order_relaxed);
        mNodeId = PW_ID_ANY;

        juce::String err;
        sys.lock();
        pw_properties* props = A.properties_new(
            PW_KEY_MEDIA_TYPE, "Audio",
            PW_KEY_MEDIA_CATEGORY, "Duplex",
            PW_KEY_MEDIA_ROLE, "DSP",
            PW_KEY_APP_NAME, ss_app_name(),
            PW_KEY_NODE_NAME, ss_app_name(),
            PW_KEY_NODE_ALWAYS_PROCESS, "true",
            nullptr);
        char tmp[64];
        snprintf(tmp, sizeof(tmp), "%d/%d", mBufFrames, mRate);
        A.properties_set(props, PW_KEY_NODE_LATENCY, tmp);
        // Same quantum ceiling as the stream devices (see makeStream).
        snprintf(tmp, sizeof(tmp), "%d/%d", std::max(mBufFrames * 2, 2048), mRate);
        A.properties_set(props, PW_KEY_NODE_MAX_LATENCY, tmp);

        mFilter = A.filter_new(sys.core(), ss_app_name(), props);
        if (mFilter == nullptr) {
            err = "pw_filter_new failed";
        } else {
            static const pw_filter_events filterEvents = [] {
                pw_filter_events e{};
                e.version = PW_VERSION_FILTER_EVENTS;
                e.state_changed = [](void* /*data*/, pw_filter_state /*old*/,
                                     pw_filter_state state, const char* error) {
                    if (state == PW_FILTER_STATE_ERROR)
                        ss_log("PipeWire filter error: %s", error != nullptr ? error : "unknown");
                    auto& s = PipeWireSystem::instance();
                    s.api.thread_loop_signal(s.loop(), false);
                };
                e.process = [](void* data, spa_io_position* position) {
                    static_cast<PatchbayAudioIODevice*>(data)->process(position);
                };
                return e;
            }();
            A.filter_add_listener(mFilter, &mFilterHook, &filterEvents, this);

            for (int i = 0; i < mNumOut + mNumIn && err.isEmpty(); ++i) {
                const bool playback = i < mNumOut;
                const int idx = playback ? i : i - mNumOut;
                snprintf(tmp, sizeof(tmp), "%s%d", playback ? "out_" : "in_", idx + 1);
                void* port = A.filter_add_port(
                    mFilter,
                    playback ? PW_DIRECTION_OUTPUT : PW_DIRECTION_INPUT,
                    PW_FILTER_PORT_FLAG_MAP_BUFFERS,
                    sizeof(PortData),
                    A.properties_new(PW_KEY_FORMAT_DSP, "32 bit float mono audio",
                                     PW_KEY_PORT_NAME, tmp,
                                     nullptr),
                    nullptr, 0);
                if (port == nullptr) {
                    err = "pw_filter_add_port failed";
                    break;
                }
                static_cast<PortData*>(port)->index = idx;
                (playback ? mOutPorts : mInPorts)[(size_t) idx] = port;
            }

            if (err.isEmpty()) {
                const int res = A.filter_connect(mFilter, PW_FILTER_FLAG_RT_PROCESS, nullptr, 0);
                if (res < 0)
                    err = "pw_filter_connect failed: " + juce::String(spa_strerror(res));
            }
        }
        sys.unlock();

        if (err.isEmpty())
            err = waitForFilter();

        if (err.isEmpty()) {
            // Registry must have seen our ports before links can name them.
            sys.roundtrip(2);
            sys.lock();
            refreshLinks();
            sys.unlock();
            sys.addChangeSink(this, [this] {
                if (!mClosing.load(std::memory_order_relaxed))
                    refreshLinks();
            });
        }

        if (err.isNotEmpty()) {
            close();
            mLastError = err;
            return err;
        }

        if (const int seen = mObservedRate.load(std::memory_order_relaxed))
            mRate = seen;

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
        mClosing.store(true, std::memory_order_relaxed);
        auto& sys = PipeWireSystem::instance();
        sys.removeChangeSink(this);
        if (sys.connected() && mFilter != nullptr) {
            sys.lock();
            dropLinks(mOutLinks);
            dropLinks(mInLinks);
            sys.api.filter_disconnect(mFilter);
            sys.api.filter_destroy(mFilter);   // also removes ports + listeners
            sys.unlock();
        }
        mFilter = nullptr;
        mFilterHook = spa_hook{};
        mNodeId = PW_ID_ANY;
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

    int getOutputLatencyInSamples() override { return mBufFrames; }
    int getInputLatencyInSamples() override  { return mBufFrames; }

private:
    struct PortData { int index; };

    struct AutoLinks {
        uint32_t targetNode = PW_ID_ANY;
        std::vector<pw_proxy*> proxies;
    };

    juce::StringArray portNames(const char* prefix) const {
        juce::StringArray names;
        for (int i = 1; i <= kPatchbayChans; ++i)
            names.add(juce::String(prefix) + juce::String(i));
        return names;
    }

    juce::String waitForFilter() {
        auto& sys = PipeWireSystem::instance();
        auto& A = sys.api;
        juce::String err;
        sys.lock();
        for (int elapsed = 0; elapsed < 5; ++elapsed) {
            const char* filterError = nullptr;
            const pw_filter_state st = A.filter_get_state(mFilter, &filterError);
            if (st == PW_FILTER_STATE_ERROR) {
                err = "PipeWire filter failed: "
                      + juce::String(filterError != nullptr ? filterError : "unknown");
                break;
            }
            if (st == PW_FILTER_STATE_PAUSED || st == PW_FILTER_STATE_STREAMING) {
                mNodeId = A.filter_get_node_id(mFilter);
                if (mNodeId != PW_ID_ANY)
                    break;
            }
            A.thread_loop_timed_wait(sys.loop(), 1);
        }
        if (err.isEmpty() && mNodeId == PW_ID_ANY)
            err = "PipeWire filter did not start";

        // ALWAYS_PROCESS keeps the graph driving us, so the true graph rate
        // arrives with the first process callback; fall back to the
        // requested rate if the graph stays quiet.
        for (int elapsed = 0;
             err.isEmpty() && elapsed < 2
                 && mObservedRate.load(std::memory_order_relaxed) == 0;
             ++elapsed)
            A.thread_loop_timed_wait(sys.loop(), 1);
        sys.unlock();
        return err;
    }

    // Auto-link maintenance. Caller must hold the thread-loop lock (the
    // change-sink path arrives with it held; open() takes it explicitly —
    // pw_thread_loop locks are recursive).
    void refreshLinks() {
        if (mNodeId == PW_ID_ANY)
            return;
        relinkSide(mOutLinks, true);
        relinkSide(mInLinks, false);
    }

    void relinkSide(AutoLinks& links, bool playback) {
        auto& sys = PipeWireSystem::instance();
        const auto target = sys.defaultTarget(playback);
        const int want = std::min({ 2, playback ? mNumOut : mNumIn,
                                    (int) target.portIds.size() });
        // Same target with links in place: leave the graph alone (including
        // any edits the user made to our links).
        if (target.nodeId == links.targetNode && (int) links.proxies.size() >= want)
            return;
        dropLinks(links);
        links.targetNode = target.nodeId;
        if (target.nodeId == PW_ID_ANY)
            return;
        for (int i = 0; i < want; ++i) {
            const uint32_t ours = sys.portGlobalId(mNodeId, !playback, i);
            if (ours == PW_ID_ANY)
                continue;
            pw_proxy* link = playback
                ? makeLink(mNodeId, ours, target.nodeId, target.portIds[(size_t) i])
                : makeLink(target.nodeId, target.portIds[(size_t) i], mNodeId, ours);
            if (link != nullptr)
                links.proxies.push_back(link);
        }
        ss_log("PipeWire patchbay: auto-linked %d %s port(s) to node %u",
               (int) links.proxies.size(), playback ? "out" : "in", target.nodeId);
    }

    void dropLinks(AutoLinks& links) {
        auto& A = PipeWireSystem::instance().api;
        for (pw_proxy* p : links.proxies)
            A.proxy_destroy(p);
        links.proxies.clear();
        links.targetNode = PW_ID_ANY;
    }

    pw_proxy* makeLink(uint32_t outNode, uint32_t outPort, uint32_t inNode, uint32_t inPort) {
        auto& sys = PipeWireSystem::instance();
        auto& A = sys.api;
        pw_properties* p = A.properties_new(nullptr, nullptr);
        char buf[16];
        snprintf(buf, sizeof(buf), "%u", outNode);
        A.properties_set(p, PW_KEY_LINK_OUTPUT_NODE, buf);
        snprintf(buf, sizeof(buf), "%u", outPort);
        A.properties_set(p, PW_KEY_LINK_OUTPUT_PORT, buf);
        snprintf(buf, sizeof(buf), "%u", inNode);
        A.properties_set(p, PW_KEY_LINK_INPUT_NODE, buf);
        snprintf(buf, sizeof(buf), "%u", inPort);
        A.properties_set(p, PW_KEY_LINK_INPUT_PORT, buf);
        auto* proxy = (pw_proxy*) pw_core_create_object(
            sys.core(), "link-factory", PW_TYPE_INTERFACE_Link, PW_VERSION_LINK,
            &p->dict, 0);
        A.properties_free(p);
        return proxy;
    }

    void process(spa_io_position* position) {
        auto& A = PipeWireSystem::instance().api;
        uint32_t n = position != nullptr ? (uint32_t) position->clock.duration
                                         : (uint32_t) mBufFrames;
        if (n == 0)
            return;
        n = std::min(n, kMaxQuantum);
        if (position != nullptr && position->clock.rate.denom != 0)
            mObservedRate.store((int) position->clock.rate.denom, std::memory_order_relaxed);

        // Unpatched ports hand back null DSP buffers: silent plane for
        // inputs, shared discard plane for outputs.
        for (int i = 0; i < mNumIn; ++i) {
            auto* b = (const float*) A.filter_get_dsp_buffer(mInPorts[(size_t) i], n);
            mInPtrs[(size_t) i] = b != nullptr ? b : mZeroPlane.data();
        }
        for (int i = 0; i < mNumOut; ++i) {
            auto* b = (float*) A.filter_get_dsp_buffer(mOutPorts[(size_t) i], n);
            mOutPtrs[(size_t) i] = b != nullptr ? b : mTrashPlane.data();
        }

        const juce::ScopedLock sl(mCallbackLock);
        if (mCallback != nullptr)
            mCallback->audioDeviceIOCallbackWithContext(
                mInPtrs.data(), mNumIn > 0 ? mNumIn : 0,
                mOutPtrs.data(), mNumOut, (int) n, {});
        else
            for (int i = 0; i < mNumOut; ++i)
                if (mOutPtrs[(size_t) i] != mTrashPlane.data())
                    memset(mOutPtrs[(size_t) i], 0, n * sizeof(float));
    }

    pw_filter* mFilter = nullptr;
    spa_hook mFilterHook{};
    uint32_t mNodeId = PW_ID_ANY;

    int mRate = 48000;
    int mBufFrames = 256;
    int mNumOut = 0, mNumIn = 0;
    bool mIsOpen = false;
    std::atomic<bool> mClosing { false };
    std::atomic<int> mObservedRate { 0 };
    juce::String mLastError;
    juce::BigInteger mActiveOut, mActiveIn;

    juce::AudioIODeviceCallback* mCallback = nullptr;
    juce::CriticalSection mCallbackLock;

    std::vector<void*> mOutPorts, mInPorts;
    std::vector<const float*> mInPtrs;
    std::vector<float*> mOutPtrs;
    std::vector<float> mZeroPlane, mTrashPlane;
    AutoLinks mOutLinks, mInLinks;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PatchbayAudioIODevice)
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

        // The patchbay is a graph citizen rather than a sink wrapper, so it
        // is offered whenever the daemon is reachable — even with no sinks
        // (its ports can still be patched to other apps).
        PwNodeInfo patchbay;
        patchbay.description = kPatchbayDeviceName;
        mOutputs.push_back(patchbay);
        outputNames.add(kPatchbayDeviceName);
        mInputs.push_back(patchbay);
        inputNames.add(kPatchbayDeviceName);
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
        if (dynamic_cast<PatchbayAudioIODevice*>(device) != nullptr)
            return (asInput ? inputNames : outputNames).indexOf(kPatchbayDeviceName);
        if (auto* d = dynamic_cast<PipeWireAudioIODevice*>(device))
            return asInput ? inputNames.indexOf(d->inputName)
                           : outputNames.indexOf(d->outputName);
        return -1;
    }

    juce::AudioIODevice* createDevice(const juce::String& outputDeviceName,
                                      const juce::String& inputDeviceName) override {
        jassert(hasScanned);
        // The patchbay's ports live on one filter node, so selecting it on
        // either side selects it for both — a stream device can't share the
        // node, and mixed pairings would reintroduce the capture ring.
        if (outputDeviceName == kPatchbayDeviceName || inputDeviceName == kPatchbayDeviceName)
            return new PatchbayAudioIODevice();
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

const char* pipeWirePatchbayDeviceName() { return kPatchbayDeviceName; }
const char* pipeWireDefaultDeviceName()  { return kDefaultDeviceName; }

#endif // __linux__ && SUPERSONIC_PIPEWIRE
