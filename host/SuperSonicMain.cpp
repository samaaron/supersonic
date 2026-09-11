// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2025-2026 Sam Aaron
/*
 * SuperSonicMain.cpp — SuperSonic's process.
 *
 * SuperSonic is a client of clockwork: it embeds the engine as a library,
 * opens the command socket with clockwork's comms client library, and stands
 * its front (front/SuperSonicFront.h) between the two, answering scsynth's
 * file verbs itself. The pieces this composes are in EngineHost.h; what is
 * added here is the order they run in, the name on the banner, and the
 * front. The engine opens no socket and reads no command line: everything a
 * launcher passes is read here.
 *
 * The CLI is scsynth's, so Sonic Pi's daemon starts this binary as it
 * started scsynth.
 */
#include <juce_core/juce_core.h>
#include "clockwork_product.h"
#include "EngineHost.h"
#include "SuperSonicFront.h"
#include "OscFront.h"
#include "ClockworkEngine.h"
#if CLOCKWORK_HAS_PLUGIN_TRACKS
#include "TrackControl.h"
#endif
#include "clockwork_config.h"

#include <cstdio>
#include <cstring>
#include <string>
#ifdef _WIN32
#include <windows.h>
#endif

using namespace supersonic_host;

int main(int argc, char* argv[]) {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif
    // ── -v / --help: before JUCE init (no COM, no audio) ─────────────────────
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "-v") == 0 || std::strcmp(argv[i], "-V") == 0) {
            // The product's version (CMakeLists.txt: project(SuperSonic VERSION)).
            fprintf(stdout, CLOCKWORK_PRODUCT_NAME " %s\n", SUPERSONIC_VERSION_STRING);
            return 0;
        }
        if (std::strcmp(argv[i], "--help") == 0 || std::strcmp(argv[i], "-h") == 0) {
            fputs(usage(CLOCKWORK_PRODUCT_NAME).c_str(), stdout);
            return 0;
        }
    }

    juce::ScopedJuceInitialiser_GUI libraryInitialiser;
    auto log = [](const std::string& s) { fprintf(stderr, CLOCKWORK_LOG_PREFIX "%s\n", s.c_str()); fflush(stderr); };

    // ── --list-devices: enumerate and exit ────────────────────────────────────
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--list-devices") == 0) {
            ClockworkEngine engine;
            engine.onDebug = [](const std::string&) {};
            engine.onReply = [](const uint8_t*, uint32_t) {};
            ClockworkEngine::Config cfg;
            cfg.headless = false;
            cfg.udpPort  = 0;
            engine.init(cfg);
            printDeviceList(engine);
            engine.shutdown();
            return 0;
        }
    }

    // ── The command line ──────────────────────────────────────────────────────
    Options o;
    std::string err;
    if (!parseArgs(argc, argv, o, &err)) { log("ERROR: " + err); return 1; }
    for (const auto& w : o.warnings) log(w);
#ifdef __APPLE__
    const std::string micStatus = applyMicPermissionGuard(o);
#endif

    installSignalHandlers();
    installCrashHandlers(CLOCKWORK_LOG_PREFIX);

    // ── Engine, transport, front ──────────────────────────────────────────────
    // This process owns the command transport; the engine owns no socket.
    // The transports are declared before the engine so they outlive its
    // teardown; the front after it, so it is torn down first — its thread
    // feeds the engine.
    CommandTransports  transports;
    shm_attach::server shmAttach;
    FrontedTransport   fronted;
    ClockworkEngine    engine;
    engine.onDebug = [](const std::string& s) {
        const size_t end = s.find_last_not_of("\r\n");
        if (end == std::string::npos) return;
        fprintf(stderr, "[synth] %.*s\n", static_cast<int>(end + 1), s.c_str());
        fflush(stderr);
    };

    SuperSonicFront* front = nullptr;
    IOscTransport* transport = transports.select(o, engine.peerPlaneSlot(),
        [&engine, &front](const uint8_t* d, uint32_t n, uint32_t token) {
            if (front && front->ingress(d, n, token)) return;   // a file verb: the front's
            engine.ingest(d, n, token);
        }, &err);
    if (!transport) { log("ERROR: " + err); return 1; }

    // The front answers through the transport the engine's replies leave by,
    // and sees those replies first. The engine keeps the transport for its
    // subscriber registry (the notify audiences are the transport's); the
    // replies themselves are pulled by this process's pump, below.
    SuperSonicFront theFront(engine, transport);
    front = &theFront;
    fronted.attach(transport, front);
    engine.setTransport(&fronted);
    o.cfg.hostDrainsEgress = true;
    EgressPump pump(engine, fronted, log);
    log(std::string("front: ") + theFront.describe());

#if CLOCKWORK_HAS_PLUGIN_TRACKS
    // Reserve the tracks' lanes before the engine boots: they are laid out
    // when memory is. The plugins live in the bridge process the engine
    // spawns once it is up (TrackControl.h).
    TrackControl::reserveLanes();
#endif

    try {
        engine.init(o.cfg);
    } catch (const std::exception& e) {
        log(std::string("ERROR: ") + e.what());
        return 1;
    } catch (...) {
        log("ERROR: unknown exception during init");
        return 1;
    }

    // The transport starts now that the engine's rings exist for it to feed.
    // An alternative transport that cannot bind is fatal: the caller chose it
    // for its guarantees and must not get a deaf server.
    if (!transports.start()) {
        log("ERROR: command transport failed to start (" + transports.description() + ")");
        fronted.detachFront();
        front = nullptr;
        engine.shutdown();
        return 1;
    }
    pump.start();
    log("egress: both rings drained here, through the client API");
    serveSegment(shmAttach, engine, o, log);

    // Inputs as asked for, once the boot-time guard has passed.
    if (o.desiredInputChannels != 0 && o.desiredInputChannels != o.cfg.numInputChannels)
        engine.setConfiguredInputChannels(o.desiredInputChannels);

    printBanner(Identity { CLOCKWORK_PRODUCT_NAME, CLOCKWORK_PRODUCT_BANNER, SUPERSONIC_VERSION_STRING },
                engine.currentDevice(), transports.description());

#ifdef __APPLE__
    // Booted with inputs off because the microphone permission was pending:
    // watch for the grant and enable them.
    bool needsInputEnable = (o.cfg.numInputChannels == 0 && micStatus != "authorized");
    runUntilShutdown(engine, log, [&] {
        if (needsInputEnable && micPermissionStatus() == "authorized") {
            log("[mic-permission] status now authorized — enabling inputs");
            engine.enableInputChannels(-1);
            needsInputEnable = false;
        }
    });
#else
    runUntilShutdown(engine, log);
#endif

    fprintf(stderr, "\n  shutting down...\n");
    // Every receiving transport stops before the engine frees its rings, and
    // the front stops being consulted before it goes.
    shmAttach.stop();
    transports.stop();
    pump.stop();
    fronted.detachFront();
    front = nullptr;
    engine.shutdown();
    return 0;
}
