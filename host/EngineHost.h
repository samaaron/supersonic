// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2025-2026 Sam Aaron
/*
 * EngineHost.h — the pieces SuperSonic's main is composed from.
 *
 * clockwork is a library: it opens no socket, parses no command line, runs
 * no event loop, and has no process shape of its own. The process around it
 * is SuperSonic's — this directory — and what such a process does is here:
 * read the scsynth-shaped flags, open the command transport with clockwork's
 * comms client library, hold what arrives until the engine is ready, serve
 * the segment, pump the platform's event loop until told to stop, and come
 * down in an order that leaves no receiving thread writing a freed ring.
 * SuperSonicMain.cpp composes these and adds what is SuperSonic's alone: its
 * name and banner, and the front it stands between the socket and the engine
 * (front/SuperSonicFront.h).
 */
#pragma once

#include "ClockworkEngine.h"
#include "IOscTransport.h"
#include "UdpOscTransport.h"
#include "StreamOscTransport.h"
#include "UdsDgramOscTransport.h"
#include "ShmTransport.h"
#include "shm_attach.hpp"

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace supersonic_host {

// Everything the command line says, and what the host derives from it.
struct Options {
    ClockworkEngine::Config cfg;          // the host's defaults, then the flags
    int         tcpPort = 0;
    std::string udsStreamPath, udsDgramPath, pipeName;
    std::string shmEndpoint;              // derived from -u when not given
    bool        shmCommands = false;
    uint32_t    maxConnections = 4;
    bool        headless = false;
    // Inputs as asked for, before any platform guard zeroes them (macOS mic
    // permission): what to restore once the guard lifts.
    int         desiredInputChannels = ClockworkEngine::kAutoChannelCount;
    std::vector<std::string> warnings;    // flags not understood — reported, never applied
};

// The flags, for --help, under the product's name.
std::string usage(const char* productName);

// argv into `out`. False, with `err`, on a combination that cannot run: two
// command transports, or a segment flag without the -u > 0 that makes one.
// -v, --help and --list-devices are the caller's: they need the product's
// name or an engine.
bool parseArgs(int argc, char* const argv[], Options& out, std::string* err);

// The command transport, chosen from the options and wired to `ingest` —
// the caller's function, so a product may stand in front of the engine.
// UDP binds and receives as soon as it is selected and holds what arrives
// until start(); a connection-oriented transport starts on start(). Both
// happen around engine.init: select before, start after.
class CommandTransports {
public:
    using Ingest = std::function<void(const uint8_t*, uint32_t, uint32_t)>;

    // The transport the engine's replies leave by, or null with `err`.
    // `peerPlaneSlot` is the engine's, for --shm-commands; null otherwise.
    IOscTransport* select(const Options& o, std::atomic<ShmPeerPlaneHeader*>* peerPlaneSlot,
                          Ingest ingest, std::string* err);
    bool start();
    void stop();
    const std::string& description() const { return mDesc; }

private:
    UdpOscTransport      mUdp;
    StreamOscTransport   mStream;
    UdsDgramOscTransport mUdsDgram;
    ShmTransport         mShm;
    Ingest               mIngest;
    std::function<bool()> mStart;
    std::string          mDesc;

    // UDP's boot queue: what arrives before the engine can ingest.
    static constexpr size_t kMaxPending = 1024;
    std::mutex        mPendingMut;
    std::vector<std::pair<std::vector<uint8_t>, uint32_t>> mPending;
    std::atomic<bool> mReady { false };
    bool              mDropWarned = false;
    std::function<void(const std::string&)> mLog;
};

// The egress pump: the host is the consumer of the engine's two egress
// rings (Config::hostDrainsEgress). One thread polls the engine's client
// handle — one read, both rings — and routes each frame to the transport
// by its route word and origin (clockwork's EgressRouter, the same table the
// engine's own gateway uses when it is the consumer). /clockwork/debug goes
// to `log`. Start after the transport starts; stop before it stops.
class EgressPump {
public:
    EgressPump(ClockworkEngine& engine, IOscTransport& transport,
               std::function<void(const std::string&)> log);
    ~EgressPump();
    void start();
    void stop();
    uint64_t framesRouted() const { return mRouted.load(); }

private:
    void run();
    ClockworkEngine&  mEngine;
    IOscTransport&    mTransport;
    std::function<void(const std::string&)> mLog;
    std::thread       mThread;
    std::atomic<bool> mStop { false };
    std::atomic<uint64_t> mRouted { 0 };
};

// Serves the engine's segment at the endpoint. Not fatal when it cannot be:
// the engine still answers every command, only its observers go blind, and
// the log says so. Returns whether it is being served.
bool serveSegment(shm_attach::server& server, ClockworkEngine& engine, const Options& o,
                  const std::function<void(const std::string&)>& log);

// The process's signals: SIGINT/SIGTERM ask for shutdown; a crash prints a
// FATAL line (and a backtrace where the platform has one) and exits.
void installSignalHandlers();
void installCrashHandlers(const char* logPrefix);
bool shutdownRequested();
void requestShutdown();

// The platform's event loop until shutdown is asked for: on macOS the
// CFRunLoop and the Cocoa event pump (AUHAL callbacks and GameController
// discovery need it); on Windows the message pump (JUCE's device-change
// window needs it); elsewhere a sleep. `tick`, if given, runs about once a
// second.
void runUntilShutdown(ClockworkEngine& engine, const std::function<void(const std::string&)>& log,
                      const std::function<void()>& tick = {});

// The banner a host prints once it is up, and the device list for --list-devices.
struct Identity {
    const char* name;
    const char* banner;      // three lines, no trailing newline
    const char* version;
};
void printBanner(const Identity& id, const CurrentDeviceInfo& dev, const std::string& transportDesc);
void printDeviceList(ClockworkEngine& engine);

#ifdef __APPLE__
// macOS: logs the microphone permission status and, unless it is
// "authorized", zeroes the inputs for boot (AUHAL hangs opening an input
// stream while the permission is pending, and a background helper cannot
// prompt). Returns the status; the host re-enables inputs when it changes.
std::string applyMicPermissionGuard(Options& o);
std::string micPermissionStatus();
#endif

} // namespace supersonic_host
