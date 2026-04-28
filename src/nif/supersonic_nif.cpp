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

#include <juce_core/juce_core.h>

#include <atomic>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>

// ─── Global state ──────────────────────────────────────────────────────────

static std::mutex g_engine_mutex;
static std::unique_ptr<SupersonicEngine> g_engine;
// JUCE runtime — initialised on first start() call (dirty scheduler)
static std::atomic<bool> g_juce_initialised{false};

// Notification context (same pattern as tau5_discovery)
struct NotificationContext {
    std::mutex mutex;
    ErlNifPid pid;
    std::atomic<bool> has_pid{false};
};
static NotificationContext g_notify;

// ─── Reply/Debug callbacks (called from worker threads) ────────────────────

static void on_osc_reply(const uint8_t* data, uint32_t size) {
    if (!g_notify.has_pid.load()) return;

    ErlNifEnv* msg_env = enif_alloc_env();
    if (!msg_env) return;

    // Copy OSC data into an Erlang binary
    ERL_NIF_TERM bin;
    uint8_t* buf = enif_make_new_binary(msg_env, size, &bin);
    if (buf) {
        memcpy(buf, data, size);

        ERL_NIF_TERM message = enif_make_tuple2(msg_env,
            enif_make_atom(msg_env, "osc_reply"),
            bin);

        {
            std::lock_guard<std::mutex> lock(g_notify.mutex);
            if (g_notify.has_pid.load()) {
                int result = enif_send(nullptr, &g_notify.pid, msg_env, message);
                if (result == 0) {
                    g_notify.has_pid.store(false);
                }
            }
        }
    }

    enif_free_env(msg_env);
}

static void on_debug(const std::string& msg) {
    if (!g_notify.has_pid.load()) return;

    ErlNifEnv* msg_env = enif_alloc_env();
    if (!msg_env) return;

    ERL_NIF_TERM message = enif_make_tuple2(msg_env,
        enif_make_atom(msg_env, "debug"),
        enif_make_string(msg_env, msg.c_str(), ERL_NIF_LATIN1));

    {
        std::lock_guard<std::mutex> lock(g_notify.mutex);
        if (g_notify.has_pid.load()) {
            int result = enif_send(nullptr, &g_notify.pid, msg_env, message);
            if (result == 0) {
                g_notify.has_pid.store(false);
            }
        }
    }

    enif_free_env(msg_env);
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
        g_engine->onReply = on_osc_reply;
        g_engine->onDebug = on_debug;
        g_engine->initialise(cfg);
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

    {
        std::lock_guard<std::mutex> notif_lock(g_notify.mutex);
        g_notify.has_pid.store(false);
    }

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

    g_engine->sendOsc(bin.data, static_cast<uint32_t>(bin.size));
    return enif_make_atom(env, "ok");
}

static ERL_NIF_TERM nif_set_notification_pid(ErlNifEnv* env, int, const ERL_NIF_TERM[]) {
    std::lock_guard<std::mutex> lock(g_notify.mutex);

    if (!enif_self(env, &g_notify.pid))
        return enif_make_badarg(env);

    g_notify.has_pid.store(true);
    return enif_make_atom(env, "ok");
}

static ERL_NIF_TERM nif_clear_notification_pid(ErlNifEnv* env, int, const ERL_NIF_TERM[]) {
    std::lock_guard<std::mutex> lock(g_notify.mutex);
    g_notify.has_pid.store(false);
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
