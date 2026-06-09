/*
 * supersonic_nif.cpp — BEAM NIF interface for SuperSonic audio engine
 *
 * Thin shim that exposes SupersonicEngine to Erlang/Elixir via NIF calls.
 * OSC messages come in via send_osc/1, replies go back to a registered
 * Erlang process via enif_send.  No UDP sockets needed.
 *
 * Follows the same patterns as tau5_discovery NIF:
 *   - Global engine instance, pointer guarded by a briefly-held mutex
 *   - PID-based notification with separate notification_mutex
 *   - Fresh ErlNifEnv per thread callback (enif_send from non-BEAM threads)
 *
 * Every NIF call is non-blocking. send_osc is a ring-buffer write (microseconds).
 * start/stop only parse + enqueue and return immediately; a single lifecycle
 * worker thread performs the slow init()/shutdown() off all BEAM schedulers and
 * posts the outcome to the calling process as {supersonic_started, Result} /
 * {supersonic_stopped, ok}. So no NIF ever blocks a scheduler — not even a dirty
 * one — and nothing needs the dirty-scheduler flag.
 */

#include "erl_nif.h"
#include "SupersonicEngine.h"
#include "IOscTransport.h"

#include <juce_core/juce_core.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

// ─── Global state ──────────────────────────────────────────────────────────

// Guards only the g_engine pointer, and is held only for the brief publish /
// unpublish / read of it — never across the slow init()/shutdown() (those run on
// the lifecycle worker with no lock held). So send_osc's acquisition is always
// uncontended-or-microseconds, even while a start/stop is in flight.
static std::mutex g_engine_mutex;
static std::unique_ptr<SupersonicEngine> g_engine;
// JUCE runtime — initialised lazily on the lifecycle worker before the first boot.
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
    bool                 midiSubscribed = false;

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
        midiSubscribed = false;
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

    // MIDI notify: a BEAM caller is addressable; delivery is gated
    // on midiSubscribed.
    void broadcastMidi(const uint8_t* data, uint32_t size) override {
        bool wanted;
        { std::lock_guard<std::mutex> lk(mSubs->mutex); wanted = mSubs->midiSubscribed; }
        if (wanted) mSubs->deliverOscReply(data, size);
    }
    bool subscribeMidi(uint32_t) override {
        std::lock_guard<std::mutex> lk(mSubs->mutex);
        mSubs->midiSubscribed = true;
        return true;
    }
    void unsubscribeMidi(uint32_t) override {
        std::lock_guard<std::mutex> lk(mSubs->mutex);
        mSubs->midiSubscribed = false;
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

// ─── Lifecycle worker ───────────────────────────────────────────────────────
//
// start()/stop() must never block a BEAM scheduler: opening the audio device and
// joining the engine's threads take real wall-clock time. So those NIFs only
// parse + enqueue a command (microseconds) and return; this single dedicated
// worker thread performs the slow init()/shutdown() off all schedulers. One
// worker ⇒ start/stop are serialised (never two engines fighting over the device)
// without any long-held lock — g_engine_mutex is taken only to publish/unpublish
// the pointer, so a concurrent send_osc never waits on lifecycle work. The
// outcome is posted to the caller as {supersonic_started, ok|{error,Reason}} /
// {supersonic_stopped, ok}.

enum class LifecycleOp { Start, Stop };
struct LifecycleCmd {
    LifecycleOp              op;
    SupersonicEngine::Config cfg;     // Start only
    ErlNifPid                pid;
    bool                     notify;  // false for VM-shutdown-initiated teardown
};

static std::mutex               g_worker_mutex;
static std::condition_variable  g_worker_cv;
static std::deque<LifecycleCmd> g_worker_queue;
static bool                     g_worker_exit = false;
// Heap-owned, never a static std::thread object: erl_nif's on_unload is not
// reliably called at VM halt, so a static std::thread could reach its destructor
// still-joinable and std::terminate(). As a pointer it's joined+deleted when
// on_unload does run, and merely leaked (no terminate) when it doesn't.
static std::thread*             g_worker_thread = nullptr;

// Post one message to a single pid from this (non-scheduler) worker thread.
template <typename MakeMsg>
static void notify_pid(const ErlNifPid& pid, MakeMsg makeMsg) {
    ErlNifEnv* env = enif_alloc_env();
    if (!env) return;
    ERL_NIF_TERM msg = makeMsg(env);
    enif_send(nullptr, const_cast<ErlNifPid*>(&pid), env, msg);
    enif_free_env(env);
}

static ERL_NIF_TERM make_started(ErlNifEnv* e, ERL_NIF_TERM result) {
    return enif_make_tuple2(e, enif_make_atom(e, "supersonic_started"), result);
}

static void worker_do_start(const LifecycleCmd& cmd) {
    // Serialised on this thread, so reading g_engine for the running-check needs
    // the lock only against a concurrent send_osc, not against another start.
    {
        std::lock_guard<std::mutex> lk(g_engine_mutex);
        if (g_engine && g_engine->isRunning()) {
            if (cmd.notify)
                notify_pid(cmd.pid, [](ErlNifEnv* e) {
                    return make_started(e, enif_make_tuple2(e,
                        enif_make_atom(e, "error"), enif_make_atom(e, "already_running")));
                });
            return;
        }
    }

    if (!g_juce_initialised.load()) {
        juce::initialiseJuce_GUI();
        g_juce_initialised.store(true);
    }

    std::unique_ptr<SupersonicEngine> engine;
    std::string err;
    try {
        engine = std::make_unique<SupersonicEngine>();
        engine->onDebug = on_debug;
        engine->setTransport(&g_transport);
        engine->init(cmd.cfg);  // SLOW — no lock held
    } catch (const std::exception& e) {
        err = e.what();
    } catch (...) {
        err = "unknown_exception";
    }

    if (!err.empty()) {
        engine.reset();
        if (cmd.notify)
            notify_pid(cmd.pid, [&](ErlNifEnv* e) {
                return make_started(e, enif_make_tuple2(e, enif_make_atom(e, "error"),
                    enif_make_string(e, err.c_str(), ERL_NIF_LATIN1)));
            });
        return;
    }

    { std::lock_guard<std::mutex> lk(g_engine_mutex); g_engine = std::move(engine); }  // brief publish

    if (cmd.notify)
        notify_pid(cmd.pid, [](ErlNifEnv* e) { return make_started(e, enif_make_atom(e, "ok")); });
}

static void worker_do_stop(const LifecycleCmd& cmd) {
    std::unique_ptr<SupersonicEngine> local;
    { std::lock_guard<std::mutex> lk(g_engine_mutex); local = std::move(g_engine); }  // brief unpublish
    if (local) local->shutdown();  // SLOW — no lock held; engine already unreachable
    g_subs.clear();                // drop the BEAM audience along with the engine

    if (cmd.notify)
        notify_pid(cmd.pid, [](ErlNifEnv* e) {
            return enif_make_tuple2(e, enif_make_atom(e, "supersonic_stopped"),
                                       enif_make_atom(e, "ok"));
        });
}

static void worker_loop() {
    for (;;) {
        LifecycleCmd cmd;
        {
            std::unique_lock<std::mutex> lk(g_worker_mutex);
            g_worker_cv.wait(lk, [] { return g_worker_exit || !g_worker_queue.empty(); });
            if (g_worker_exit) return;  // drop any still-queued ops at VM shutdown
            cmd = std::move(g_worker_queue.front());
            g_worker_queue.pop_front();
        }
        if (cmd.op == LifecycleOp::Start) worker_do_start(cmd);
        else                              worker_do_stop(cmd);
    }
}

static void worker_enqueue(LifecycleCmd cmd) {
    { std::lock_guard<std::mutex> lk(g_worker_mutex); g_worker_queue.push_back(std::move(cmd)); }
    g_worker_cv.notify_one();
}

// ─── NIF functions ─────────────────────────────────────────────────────────

static ERL_NIF_TERM nif_is_loaded(ErlNifEnv* env, int, const ERL_NIF_TERM[]) {
    return enif_make_atom(env, "true");
}

// Async: parse the config (fast, needs the calling env), then hand the slow boot
// to the lifecycle worker. Returns `ok` immediately; the outcome arrives as
// {supersonic_started, ok | {error, Reason}} to the calling process.
static ERL_NIF_TERM nif_start(ErlNifEnv* env, int argc, const ERL_NIF_TERM argv[]) {
    if (argc != 1) return enif_make_badarg(env);

    LifecycleCmd cmd;
    cmd.op = LifecycleOp::Start;
    cmd.cfg.udpPort = 0;  // NIF mode: no UDP needed (OS picks ephemeral port, unused)
    parse_config(env, argv[0], cmd.cfg);
    enif_self(env, &cmd.pid);
    cmd.notify = true;
    worker_enqueue(std::move(cmd));

    return enif_make_atom(env, "ok");  // accepted; result delivered as a message
}

// Async: hand the slow teardown (thread joins, device close) to the lifecycle
// worker. Returns `ok` immediately; completion arrives as {supersonic_stopped, ok}.
static ERL_NIF_TERM nif_stop(ErlNifEnv* env, int, const ERL_NIF_TERM[]) {
    LifecycleCmd cmd;
    cmd.op = LifecycleOp::Stop;
    enif_self(env, &cmd.pid);
    cmd.notify = true;
    worker_enqueue(std::move(cmd));

    return enif_make_atom(env, "ok");
}

static ERL_NIF_TERM nif_send_osc(ErlNifEnv* env, int argc, const ERL_NIF_TERM argv[]) {
    if (argc != 1) return enif_make_badarg(env);

    ErlNifBinary bin;
    if (!enif_inspect_binary(env, argv[0], &bin))
        return enif_make_badarg(env);

    // Brief lock: the worker never holds g_engine_mutex across slow lifecycle
    // work, so this never waits on a boot/teardown — the call stays a fast,
    // scheduler-safe ring write. The lock also keeps stop from freeing the
    // engine mid-send: stop must take the lock to unpublish g_engine, so it
    // blocks until this sendOSC completes.
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
    // Spin up the lifecycle worker (idle until the first start). JUCE/audio init
    // is deferred to the worker on the first boot.
    g_worker_exit = false;
    g_worker_thread = new std::thread(worker_loop);
    return 0;
}

static void on_unload(ErlNifEnv*, void*) {
    // Stop the lifecycle worker: it finishes any in-flight op, then exits without
    // touching the (shutting-down) BEAM, so no enif_send races VM teardown.
    {
        std::lock_guard<std::mutex> lk(g_worker_mutex);
        g_worker_exit = true;
    }
    g_worker_cv.notify_one();
    if (g_worker_thread) {
        if (g_worker_thread->joinable())
            g_worker_thread->join();
        delete g_worker_thread;
        g_worker_thread = nullptr;
    }

    // Tear down a still-running engine inline (VM is exiting; blocking is fine).
    std::unique_ptr<SupersonicEngine> local;
    { std::lock_guard<std::mutex> lk(g_engine_mutex); local = std::move(g_engine); }
    if (local)
        local->shutdown();

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

// Every function is non-blocking, so none need dirty scheduling: start/stop just
// enqueue to the lifecycle worker and return; send_osc is a ring write.
static ErlNifFunc nif_funcs[] = {
    {"is_nif_loaded",          0, nif_is_loaded,              0},
    {"start",                  1, nif_start,                  0},
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
