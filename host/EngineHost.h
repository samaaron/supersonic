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
 * (front/supersonic_commands.h).
 */
#pragma once

#include "ClockworkEngine.h"
#include "IOscTransport.h"
#include "StreamOscTransport.h"
#include "ShmTransport.h"
#include "shm_attach.hpp"
#include "clockwork_comms_secret.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace supersonic_host {

// Everything the command line says, and what the host derives from it.
struct Options {
    ClockworkEngine::Config cfg;          // the host's defaults, then the flags
    int         shmPort = 0;              // -u: names the segment and its endpoint; 0 = no segment
    int         tcpPort = 0;
    std::string udsStreamPath, pipeName;
    std::string shmEndpoint;              // derived from -u when not given
    bool        shmCommands = false;
    bool        headless = false;
    // The per-boot secret every command port and the attach endpoint ask a
    // client for (clockwork_comms_secret.h): read from stdin by readSecret.
    std::unique_ptr<ClockworkCommsSecret, void (*)(ClockworkCommsSecret*)> secret { nullptr, &clockwork_comms_secret_free };
    // Inputs as asked for, before any platform guard zeroes them (macOS mic
    // permission): what to restore once the guard lifts.
    int         desiredInputChannels = ClockworkEngine::kAutoChannelCount;
    // scsynth's -D: load the synthdef directory at boot (through
    // supersonic::Commands; the engine reads no files).
    bool        loadDefinitionsAtBoot = false;
    std::vector<std::string> warnings;    // flags not understood — reported, never applied
};

// The flags, for --help, under the product's name.
std::string usage(const char* productName);

// argv into `out`. False, with `err`, on a combination that cannot run: two
// command transports or none, or a segment flag without the -u > 0 that
// makes one. -v, --help and --list-devices are the caller's: they need the
// product's name or an engine.
bool parseArgs(int argc, char* const argv[], Options& out, std::string* err);

// The secret from `in` (the process's stdin): one line of 64 hex digits.
// False, with `err`, for anything else; a transport does not start without it.
bool readSecret(Options& out, FILE* in, std::string* err);

// The command transport, chosen from the options and wired to `ingest` —
// the caller's function, so a product may stand in front of the engine.
// Selected before engine.init and started after it, once the rings exist.
class CommandTransports {
public:
    using Ingest = std::function<void(const uint8_t*, uint32_t, uint32_t)>;

    // The transport the engine's replies leave by, or null with `err`.
    // `peerPlaneSlot` is the engine's, for --shm-commands; null otherwise.
    IOscTransport* select(const Options& o, std::atomic<ShmPeerPlaneHeader*>* peerPlaneSlot,
                          Ingest ingest, std::string* err);
    bool start();
    void stop();
    // The transport and its address, for a message about it (a bind that
    // failed). kind() leaves the address out, for the banner: Sonic Pi picks
    // the port at random on every boot.
    const std::string& description() const { return mDesc; }
    const std::string& kind() const { return mKind; }

private:
    StreamOscTransport   mStream;
    ShmTransport         mShm;
    Ingest               mIngest;
    std::function<bool()> mStart;
    std::string          mDesc;
    std::string          mKind;
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
// Two of the banner's lines: the product and version, and what was compiled
// in. The answer to /clockwork/summary, which a GUI shows under a logo of its
// own (the device it shows elsewhere).
std::string bannerSummary(const Identity& id);
void printBanner(const Identity& id, const CurrentDeviceInfo& dev, const std::string& transportKind);
void printDeviceList(ClockworkEngine& engine);

#ifdef __APPLE__
// macOS: logs the microphone permission status and, unless it is
// "authorized", zeroes the inputs for boot (AUHAL hangs opening an input
// stream while the permission is pending, and a background helper cannot
// prompt). Returns the status; the host re-enables inputs when it changes.
std::string applyMicPermissionGuard(Options& o);
// Whether inputs, held off at boot by the guard, wait to be turned on when the
// permission is granted.
bool inputsWaitForPermission(int desiredInputChannels, const std::string& status);
std::string micPermissionStatus();
#endif

} // namespace supersonic_host
