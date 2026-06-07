/*
 * supersonic_nif.cpp — BEAM NIF interface for SuperSonic audio engine
 *
 * Thin shim that exposes SupersonicEngine to Erlang/Elixir via NIF calls.
 * OSC messages come in via send_osc/1, replies go back to a registered
 * Erlang process via enif_send.  No UDP sockets needed.
 *
 * Follows the same patterns as tau5_discovery NIF:
 *   - Global engine instance protected by mutex
 *   - PID-based notification with separate notification_mutex
 *   - Dirty I/O scheduling for start (heavy JUCE/audio init)
 *   - Fresh ErlNifEnv per thread callback (enif_send from non-BEAM threads)
 */

#include "erl_nif.h"
#include "SupersonicEngine.h"
#include "IOscTransport.h"

#include <juce_core/juce_core.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

// ─── Global state ──────────────────────────────────────────────────────────

static std::mutex g_engine_mutex;
static std::unique_ptr<SupersonicEngine> g_engine;
// JUCE runtime — initialised on first start() call (dirty scheduler)
static std::atomic<bool> g_juce_initialised{false};

// ─── Subscriber registry ────────────────────────────────────────────────────
//
// The BEAM audience for engine egress. Any number of Erlang processes register
// (set_notification_pid); each receives every reply, broadcast and debug line as
// an {osc_reply, Binary} / {debug, String} message. A process that has died is
// detected lazily — enif_send returns 0 — and only that pid is dropped, never the
// whole audience (the old single-pid context silently muted all replies on the
// first failure). This is at least as good as native UDP, which can't detect a
// dead peer at all and keeps stale targets until an explicit unsubscribe.
//
// notifyTokens/notifyPorts/linkSubscribed mirror the transport's subscriber gates
// so the engine knows whether to bother emitting device- and Link-notify traffic;
// they don't change WHO receives (always the registered pids), only WHETHER the
// engine produces the optional broadcasts. Touched from BEAM scheduler threads
// (register/clear) and the NRT gateway thread (deliver), so all access is locked.
struct Subscribers {
    mutable std::mutex   mutex;
    std::vector<ErlNifPid> pids;
    std::set<uint32_t>   notifyTokens;   // gates hasNotifySubscribers()
    std::set<int>        notifyPorts;
    bool                 linkSubscribed = false;

    void addPid(const ErlNifPid& p) {
        std::lock_guard<std::mutex> lk(mutex);
        for (const auto& e : pids)
            if (enif_compare_pids(&e, &p) == 0) return;  // idempotent
        pids.push_back(p);
    }
    void removePid(const ErlNifPid& p) {
        std::lock_guard<std::mutex> lk(mutex);
        erasePid(p);
    }
    void clear() {
        std::lock_guard<std::mutex> lk(mutex);
        pids.clear();
        notifyTokens.clear();
        notifyPorts.clear();
        linkSubscribed = false;
    }

    // Frame {osc_reply, <<bytes>>} and fan out to every registered pid.
    bool deliverOscReply(const uint8_t* data, uint32_t size) {
        return deliverAll([&](ErlNifEnv* env) {
            ERL_NIF_TERM bin;
            uint8_t* buf = enif_make_new_binary(env, size, &bin);
            if (buf) memcpy(buf, data, size);
            return enif_make_tuple2(env, enif_make_atom(env, "osc_reply"), bin);
        });
    }
    // Frame {debug, "..."} and fan out to every registered pid.
    void deliverDebug(const std::string& msg) {
        deliverAll([&](ErlNifEnv* env) {
            return enif_make_tuple2(env, enif_make_atom(env, "debug"),
                enif_make_string(env, msg.c_str(), ERL_NIF_LATIN1));
        });
    }

private:
    // Caller must hold mutex.
    void erasePid(const ErlNifPid& p) {
        pids.erase(std::remove_if(pids.begin(), pids.end(),
            [&](const ErlNifPid& e) { return enif_compare_pids(&e, &p) == 0; }),
            pids.end());
    }

    // Build a fresh message env per pid (enif_send invalidates it), send, and
    // evict pids whose process has gone. Returns true if any pid was registered.
    template <typename MakeMsg>
    bool deliverAll(MakeMsg makeMsg) {
        std::lock_guard<std::mutex> lk(mutex);
        if (pids.empty()) return false;
        std::vector<ErlNifPid> dead;
        for (const auto& p : pids) {
            ErlNifEnv* env = enif_alloc_env();
            if (!env) continue;
            ERL_NIF_TERM msg = makeMsg(env);
            int ok = enif_send(nullptr, const_cast<ErlNifPid*>(&p), env, msg);
            enif_free_env(env);
            if (ok == 0) dead.push_back(p);
        }
        for (const auto& d : dead) erasePid(d);
        return true;
    }
};
static Subscribers g_subs;

// ─── NifTransport — the engine→BEAM egress boundary ─────────────────────────
//
// The BEAM analogue of OscUdpServer: an IOscTransport injected into the engine
// (via setTransport) so the NIF is a first-class peer, not a shared-memory
// observer. Unlike CallbackTransport it delivers Link broadcasts and the
// networkOnly Link snapshot — an out-of-process BEAM client can only see what
// enif_send hands it. The NRT gateway is the sole caller of these methods, but
// the subscriber registry it forwards to is locked (BEAM threads mutate it).
class NifTransport : public IOscTransport {
public:
    explicit NifTransport(Subscribers* subs) : mSubs(subs) {}

    // token is always 0 (in-process); the audience is the registered pids.
    // networkOnly is ignored: the NIF is a real peer, so Link snapshots ship.
    bool send(uint32_t, const uint8_t* data, uint32_t size, bool /*networkOnly*/) override {
        return mSubs->deliverOscReply(data, size);
    }
    void broadcastNotify(const uint8_t* data, uint32_t size) override {
        mSubs->deliverOscReply(data, size);
    }
    void broadcastLink(const uint8_t* data, uint32_t size) override {
        bool wanted;
        { std::lock_guard<std::mutex> lk(mSubs->mutex); wanted = mSubs->linkSubscribed; }
        if (wanted) mSubs->deliverOscReply(data, size);
    }

    bool hasNotifySubscribers() const override {
        std::lock_guard<std::mutex> lk(mSubs->mutex);
        return !mSubs->notifyTokens.empty() || !mSubs->notifyPorts.empty();
    }
    bool subscribeNotify(uint32_t token) override {
        std::lock_guard<std::mutex> lk(mSubs->mutex);
        return mSubs->notifyTokens.insert(token).second;
    }
    void subscribeNotifyPort(int port) override {
        std::lock_guard<std::mutex> lk(mSubs->mutex);
        mSubs->notifyPorts.insert(port);
    }
    void unsubscribeNotify(uint32_t token) override {
        std::lock_guard<std::mutex> lk(mSubs->mutex);
        mSubs->notifyTokens.erase(token);
    }
    void clearNotify() override {
        std::lock_guard<std::mutex> lk(mSubs->mutex);
        mSubs->notifyTokens.clear();
        mSubs->notifyPorts.clear();
    }

    // A BEAM caller is addressable (unlike a pure in-process observer), so Link
    // notify is supported; delivery is gated on linkSubscribed in broadcastLink.
    bool subscribeLink(uint32_t) override {
        std::lock_guard<std::mutex> lk(mSubs->mutex);
        mSubs->linkSubscribed = true;
        return true;
    }
    void unsubscribeLink(uint32_t) override {
        std::lock_guard<std::mutex> lk(mSubs->mutex);
        mSubs->linkSubscribed = false;
    }

private:
    Subscribers* mSubs;
};
static NifTransport g_transport(&g_subs);

// ─── Debug callback (called from worker threads) ────────────────────────────
// Debug rides the engine's onDebug channel (not the transport); fan it out to
// the same BEAM audience as replies.
static void on_debug(const std::string& msg) {
    g_subs.deliverDebug(msg);
}

// ─── Config parsing helper ─────────────────────────────────────────────────

static void parse_config(ErlNifEnv* env, ERL_NIF_TERM map,
                          SupersonicEngine::Config& cfg) {
    ERL_NIF_TERM key, value;
    ErlNifMapIterator iter;

    if (!enif_map_iterator_create(env, map, &iter, ERL_NIF_MAP_ITERATOR_FIRST))
        return;

    while (enif_map_iterator_get_pair(env, &iter, &key, &value)) {
        char key_buf[64];
        int int_val;
        if (enif_get_atom(env, key, key_buf, sizeof(key_buf), ERL_NIF_LATIN1)) {
            if (strcmp(key_buf, "sample_rate") == 0 && enif_get_int(env, value, &int_val))
                cfg.sampleRate = int_val;
            else if (strcmp(key_buf, "num_output_channels") == 0 && enif_get_int(env, value, &int_val))
                cfg.numOutputChannels = int_val;
            else if (strcmp(key_buf, "num_input_channels") == 0 && enif_get_int(env, value, &int_val))
                cfg.numInputChannels = int_val;
            else if (strcmp(key_buf, "buffer_size") == 0 && enif_get_int(env, value, &int_val))
                cfg.bufferSize = int_val;
            else if (strcmp(key_buf, "num_buffers") == 0 && enif_get_int(env, value, &int_val))
                cfg.numBuffers = int_val;
            else if (strcmp(key_buf, "max_nodes") == 0 && enif_get_int(env, value, &int_val))
                cfg.maxNodes = int_val;
            else if (strcmp(key_buf, "num_audio_bus_channels") == 0 && enif_get_int(env, value, &int_val))
                cfg.numAudioBusChannels = int_val;
            else if (strcmp(key_buf, "num_control_bus_channels") == 0 && enif_get_int(env, value, &int_val))
                cfg.numControlBusChannels = int_val;
            else if (strcmp(key_buf, "max_wire_bufs") == 0 && enif_get_int(env, value, &int_val))
                cfg.maxWireBufs = int_val;
            else if (strcmp(key_buf, "max_graph_defs") == 0 && enif_get_int(env, value, &int_val))
                cfg.maxGraphDefs = int_val;
            else if (strcmp(key_buf, "real_time_memory_size") == 0 && enif_get_int(env, value, &int_val))
                cfg.realTimeMemorySize = int_val;
            else if (strcmp(key_buf, "num_rgens") == 0 && enif_get_int(env, value, &int_val))
                cfg.numRGens = int_val;
            else if (strcmp(key_buf, "headless") == 0) {
                char atom_buf[16];
                if (enif_get_atom(env, value, atom_buf, sizeof(atom_buf), ERL_NIF_LATIN1))
                    cfg.headless = (strcmp(atom_buf, "true") == 0);
            }
        }
        enif_map_iterator_next(env, &iter);
    }
    enif_map_iterator_destroy(env, &iter);
}

// ─── NIF functions ─────────────────────────────────────────────────────────

static ERL_NIF_TERM nif_is_loaded(ErlNifEnv* env, int, const ERL_NIF_TERM[]) {
    return enif_make_atom(env, "true");
}

static ERL_NIF_TERM nif_start(ErlNifEnv* env, int argc, const ERL_NIF_TERM argv[]) {
    if (argc != 1) return enif_make_badarg(env);

    // Initialise JUCE runtime if not already done
    if (!g_juce_initialised.load()) {
        juce::initialiseJuce_GUI();
        g_juce_initialised.store(true);
    }

    std::lock_guard<std::mutex> lock(g_engine_mutex);

    if (g_engine && g_engine->isRunning()) {
        return enif_make_tuple2(env,
            enif_make_atom(env, "error"),
            enif_make_atom(env, "already_running"));
    }

    // Parse config from Erlang map
    SupersonicEngine::Config cfg;
    cfg.udpPort = 0;  // NIF mode: no UDP needed (OS picks ephemeral port, unused)
    parse_config(env, argv[0], cfg);

    try {
        g_engine = std::make_unique<SupersonicEngine>();
        g_engine->onDebug = on_debug;
        // Inject the BEAM transport before init() — replies/broadcasts then route
        // to the registered pids instead of the default in-process CallbackTransport.
        g_engine->setTransport(&g_transport);
        g_engine->init(cfg);
    } catch (const std::exception& e) {
        g_engine.reset();
        return enif_make_tuple2(env,
            enif_make_atom(env, "error"),
            enif_make_string(env, e.what(), ERL_NIF_LATIN1));
    } catch (...) {
        g_engine.reset();
        return enif_make_tuple2(env,
            enif_make_atom(env, "error"),
            enif_make_atom(env, "unknown_exception"));
    }

    return enif_make_atom(env, "ok");
}

static ERL_NIF_TERM nif_stop(ErlNifEnv* env, int, const ERL_NIF_TERM[]) {
    std::lock_guard<std::mutex> lock(g_engine_mutex);

    if (g_engine) {
        g_engine->shutdown();
        g_engine.reset();
    }

    g_subs.clear();

    return enif_make_atom(env, "ok");
}

static ERL_NIF_TERM nif_send_osc(ErlNifEnv* env, int argc, const ERL_NIF_TERM argv[]) {
    if (argc != 1) return enif_make_badarg(env);

    ErlNifBinary bin;
    if (!enif_inspect_binary(env, argv[0], &bin))
        return enif_make_badarg(env);

    std::lock_guard<std::mutex> lock(g_engine_mutex);
    if (!g_engine || !g_engine->isRunning()) {
        return enif_make_tuple2(env,
            enif_make_atom(env, "error"),
            enif_make_atom(env, "not_running"));
    }

    g_engine->sendOSC(bin.data, static_cast<uint32_t>(bin.size));
    return enif_make_atom(env, "ok");
}

static ERL_NIF_TERM nif_set_notification_pid(ErlNifEnv* env, int, const ERL_NIF_TERM[]) {
    ErlNifPid self;
    if (!enif_self(env, &self))
        return enif_make_badarg(env);

    g_subs.addPid(self);
    return enif_make_atom(env, "ok");
}

static ERL_NIF_TERM nif_clear_notification_pid(ErlNifEnv* env, int, const ERL_NIF_TERM[]) {
    // Unregister only the calling process; other subscribers keep receiving.
    ErlNifPid self;
    if (!enif_self(env, &self))
        return enif_make_badarg(env);

    g_subs.removePid(self);
    return enif_make_atom(env, "ok");
}

// ─── NIF lifecycle ─────────────────────────────────────────────────────────

static int on_load(ErlNifEnv*, void**, ERL_NIF_TERM) {
    return 0;  // JUCE init deferred to first start() call (dirty scheduler)
}

static void on_unload(ErlNifEnv*, void*) {
    {
        std::lock_guard<std::mutex> lock(g_engine_mutex);
        if (g_engine) {
            g_engine->shutdown();
            g_engine.reset();
        }
    }
    g_subs.clear();
#ifdef __APPLE__
    // Do NOT call shutdownJuce_GUI() on macOS.  The teardown path
    // (MessageManager::doPlatformSpecificShutdown → ~AppDelegate) calls
    // NSRunLoop/NSNotificationCenter/NSApp APIs that require the main
    // thread.  on_unload runs on an arbitrary BEAM scheduler, causing
    // an abort (trap 6).
    //
    // This is safe because on_unload only fires at VM shutdown (we pass
    // NULL for both reload and upgrade in ERL_NIF_INIT, so hot reload
    // is not supported).  The OS reclaims all process memory at exit.
#else
    if (g_juce_initialised.load()) {
        juce::shutdownJuce_GUI();
        g_juce_initialised.store(false);
    }
#endif
}

// ─── Function table ────────────────────────────────────────────────────────

static ErlNifFunc nif_funcs[] = {
    {"is_nif_loaded",          0, nif_is_loaded,              0},
    {"start",                  1, nif_start,                  ERL_NIF_DIRTY_JOB_IO_BOUND},
    {"stop",                   0, nif_stop,                   0},
    {"send_osc",               1, nif_send_osc,               0},
    {"set_notification_pid",   0, nif_set_notification_pid,   0},
    {"clear_notification_pid", 0, nif_clear_notification_pid, 0},
};

// Hot upgrade intentionally disabled: JUCE/NSApplication teardown is
// not safe from arbitrary BEAM schedulers (see on_unload). The two
// NULLs below are the `reload' and `upgrade' slots. Rationale and
// user-facing docs: supersonic.erl module @doc.
ERL_NIF_INIT(supersonic, nif_funcs, on_load, NULL, NULL, on_unload)
