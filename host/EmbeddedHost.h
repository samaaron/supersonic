// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Sam Aaron
/*
 * EmbeddedHost.h — SuperSonic inside your process.
 *
 * SuperSonic is clockwork hosting scsynth, and clockwork is a library: it
 * opens no socket and has no process shape. SuperSonicMain.cpp composes a
 * process around it (sockets, a segment endpoint, a run loop); the NIF
 * composes a BEAM binding. This is the third composition, for an application
 * that links the engine in: a Qt GUI, a DAW plugin's standalone, a game.
 *
 *   supersonic::EmbeddedHost host;
 *   ClockworkEngine::Config cfg;            // the device, the guest's options
 *   cfg.udpPort = 57110;                    // > 0: a shared-memory segment for scopes and metrics (nothing is bound)
 *   cfg.inboxBytes = size_t(512) << 20;     // where samples are staged: hold the largest you will load (address space only)
 *   host.boot(cfg, { onMessage, onLog }, &err);
 *   host.loadDefinitions("/path/to/synthdefs", &why);
 *   host.send(bytes, n);                    // any OSC message or bundle, as to scsynth over UDP
 *   ...
 *   host.shutdown();
 *
 * What boot() composes: the engine; supersonic::Commands in front of it,
 * which answers scsynth's file verbs (/b_allocRead, /d_load, /d_loadDir ...)
 * and the recording verbs by reading and writing the files itself, so the
 * engine stays file-free; and an in-process transport, so every reply and
 * broadcast the engine makes arrives on ONE callback instead of a socket.
 *
 * ── Threads ───────────────────────────────────────────────────────────────
 * The rules clockwork's rings impose, stated once:
 *
 *   boot(), shutdown()     the calling thread, blocking (the audio device
 *                          opens; every definition in a directory loads).
 *                          Not the audio thread; not a thread that must
 *                          stay responsive (a GUI's).
 *   send()                 any thread, never the audio thread. A ring write:
 *                          microseconds. Serialised inside.
 *   onMessage              the engine's NRT gateway thread (one thread, in
 *                          order). Do little there: decode, hand on. Never
 *                          call boot() or shutdown() from it. send() from it
 *                          is fine.
 *   onLog                  the engine's threads, including the audio thread
 *                          (where it is lock-free and may drop). Do nothing
 *                          slow there.
 *   loadDefinitions()      the calling thread, blocking until the engine has
 *                          answered for each file. After boot().
 *   dupSegmentHandle()     any thread; hand the handle to shm_segment_client
 *                          (shm_segment.hpp), which owns it from there.
 *
 * ── One engine per process ───────────────────────────────────────────────
 * clockwork is a process singleton: one EmbeddedHost at a time.
 */
#pragma once

#include "ClockworkEngine.h"
#include "shm_segment.hpp"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>

namespace supersonic {

class Commands;

class EmbeddedHost {
public:
    struct Callbacks {
        // Every reply and broadcast the engine makes, as OSC bytes (a message
        // or a bundle), on the engine's gateway thread.
        std::function<void(const uint8_t* data, uint32_t size)> onMessage;
        // The engine's debug lines, one per call, without a trailing newline.
        std::function<void(const std::string&)> onLog;
    };

    EmbeddedHost();
    ~EmbeddedHost();
    EmbeddedHost(const EmbeddedHost&) = delete;
    EmbeddedHost& operator=(const EmbeddedHost&) = delete;

    // Brings the engine up. Blocks for the boot. False, with `err`, when the
    // engine could not start (no device, an option the guest refused ...).
    // `cfg.udpPort` > 0 creates the shared-memory segment (dupSegmentHandle);
    // no socket is ever bound by this host.
    bool boot(const ClockworkEngine::Config& cfg, Callbacks callbacks, std::string* err);
    // Takes the engine down in the order the rings need. Idempotent.
    void shutdown();
    bool running() const { return mRunning.load(); }

    // One OSC packet for the engine: the front first (a file verb, a recording
    // verb), else the engine's ingress. False when refused: no engine, a ring
    // momentarily full (try again), or a packet too large for the ring (bulk
    // belongs in the inbox).
    bool send(const uint8_t* data, uint32_t size);

    // Every definition in `dir`, as scsynth's -D: blocks until the engine has
    // answered for each. False, with `why`, when the directory holds none or
    // the engine does not answer.
    bool loadDefinitions(const std::string& dir, std::string* why);

    // The segment's handle, duplicated for a reader in this process
    // (shm_segment_client takes ownership); invalid when there is no segment.
    detail_shm_segment::shm_native_handle dupSegmentHandle() const;

    // The engine and the front, for what this surface does not say: the
    // device (engine().currentDevice()), the recording state (front().recording()).
    ClockworkEngine& engine();
    Commands& front();

    // The product and version this was built as, and what was compiled in.
    std::string summary() const;

private:
    struct Impl;
    std::unique_ptr<Impl> mImpl;
    std::atomic<bool> mRunning { false };
    std::mutex mSendMutex;
};

} // namespace supersonic
