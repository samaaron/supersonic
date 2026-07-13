/*
 * Main.cpp — SuperSonic server entry point (scsynth drop-in replacement)
 *
 * Accepts scsynth command-line flags so Sonic Pi (and other SC clients)
 * can launch this binary exactly like scsynth.exe.
 *
 * JUCE provides the audio driver (WASAPI/CoreAudio/ALSA); SuperSonic's
 * scsynth core handles synthesis. OSC commands arrive over UDP by default,
 * or over TCP / Unix sockets / a named pipe via the --tcp/--uds/--uds-dgram/
 * --pipe flags.
 */
#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_core/juce_core.h>
#include "SupersonicEngine.h"
#include "ShmTransport.h"
#include "StreamOscTransport.h"
#include "UdpOscTransport.h"
#include "UdsDgramOscTransport.h"
#include "supersonic_config.h"

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#ifdef _WIN32
#include <windows.h>
#else
#include <execinfo.h>
#include <unistd.h>
#endif
#ifdef __APPLE__
#include <CoreFoundation/CoreFoundation.h>
#include "MicPermission.h"
#endif

static constexpr const char* VERSION = SUPERSONIC_VERSION_STRING;

static std::atomic<bool> gShutdownRequested{false};

static void signalHandler(int sig) {
    (void)sig;
    gShutdownRequested.store(true);
}

static void crashHandler(int sig) {
    const char* name = "UNKNOWN";
    if (sig == SIGSEGV) name = "SIGSEGV";
#ifndef _WIN32
    else if (sig == SIGBUS)  name = "SIGBUS";
#endif
    else if (sig == SIGABRT) name = "SIGABRT";
    else if (sig == SIGFPE)  name = "SIGFPE";

    fprintf(stderr, "\n[supersonic] FATAL: %s (signal %d)\n", name, sig);

#ifndef _WIN32
    void* frames[64];
    int n = backtrace(frames, 64);
    fprintf(stderr, "[supersonic] Backtrace (%d frames):\n", n);
    backtrace_symbols_fd(frames, n, STDERR_FILENO);
#endif

    fprintf(stderr, "[supersonic] Exiting due to crash.\n");
    fflush(stderr);
    _exit(128 + sig);
}

// Helper: get next arg value or nullptr
static const char* nextArg(int i, int argc, char* argv[]) {
    return (i + 1 < argc) ? argv[i + 1] : nullptr;
}

static void printBanner(const char* version, const CurrentDeviceInfo& dev,
                        const std::string& transportDesc) {
    fprintf(stderr,
        "\n"
        "  ░█▀▀░█░█░█▀█░█▀▀░█▀▄░█▀▀░█▀█░█▀█░▀█▀░█▀▀\n"
        "  ░▀▀█░█░█░█▀▀░█▀▀░█▀▄░▀▀█░█░█░█░█░░█░░█░░\n"
        "  ░▀▀▀░▀▀▀░▀░░░▀▀▀░▀░▀░▀▀▀░▀▀▀░▀░▀░▀▀▀░▀▀▀\n"
        "\n"
        "  SuperSonic v%s (SuperCollider scsynth-compatible 3.14.1)\n"
        "\n", version);

    // What this build was compiled with (feature flags can disable things).
    fprintf(stderr, "  Compiled:");
#if SUPERSONIC_SYNTH
    fprintf(stderr, " synth");
#endif
#if SUPERSONIC_LINK
    fprintf(stderr, " Link");
#endif
#if SUPERSONIC_LINK && SUPERSONIC_SYNTH
    fprintf(stderr, " Link-Audio");
#endif
#if SUPERSONIC_MIDI
    fprintf(stderr, " MIDI");
#endif
#if SUPERSONIC_GAMEPAD
    fprintf(stderr, " Gamepad");
#endif
    fprintf(stderr, "\n");

    if (!dev.name.empty()) {
        // "out A/M in B/N" — A channels currently routed / M available on
        // the device (likewise for inputs). M shows the device's real
        // capability from CoreAudio; A is what scsynth is actually
        // using (capped by daemon's -i / -o args, currently 2/2 by
        // default). Useful for MOTU-class devices with 8+ channels
        // where "active 2" doesn't mean "only 2 available".
        char outStr[32], inStr[32];
        if (dev.maxOutputChannels > 0 && dev.maxOutputChannels != dev.activeOutputChannels)
            snprintf(outStr, sizeof(outStr), "%d/%d", dev.activeOutputChannels, dev.maxOutputChannels);
        else
            snprintf(outStr, sizeof(outStr), "%d", dev.activeOutputChannels);
        if (dev.maxInputChannels > 0 && dev.maxInputChannels != dev.activeInputChannels)
            snprintf(inStr, sizeof(inStr), "%d/%d", dev.activeInputChannels, dev.maxInputChannels);
        else
            snprintf(inStr, sizeof(inStr), "%d", dev.activeInputChannels);
        fprintf(stderr,
            "  %s (%s)\n"
            "  %d Hz | block %d | buffer %d | out %s | in %s\n",
            dev.name.c_str(), dev.typeName.c_str(),
            static_cast<int>(dev.activeSampleRate), dev.controlBlockSize, dev.activeBufferSize,
            outStr, inStr);
    } else {
        fprintf(stderr, "  headless (no audio device)\n");
    }

    fprintf(stderr, "  %s\n\n", transportDesc.c_str());
    fflush(stderr);
}

static void printDeviceList(SupersonicEngine& engine) {
    auto devices = engine.listDevices();
    auto current = engine.currentDevice();

    fprintf(stdout, "\n  Audio Devices\n");
    fprintf(stdout, "  ─────────────\n\n");

    for (auto& dev : devices) {
        if (dev.isWirelessTransport()) continue;
        bool isCurrent = (dev.name == current.name && dev.typeName == current.typeName);
        fprintf(stdout, "  %s %s : %s\n", isCurrent ? "▸" : " ",
                dev.typeName.c_str(), dev.name.c_str());

        if (dev.maxOutputChannels > 0 || dev.maxInputChannels > 0)
            fprintf(stdout, "      channels: %d out, %d in\n",
                    dev.maxOutputChannels, dev.maxInputChannels);

        if (!dev.availableSampleRates.empty()) {
            fprintf(stdout, "      rates:   ");
            for (size_t i = 0; i < dev.availableSampleRates.size(); ++i) {
                if (i > 0) fprintf(stdout, ", ");
                fprintf(stdout, "%.0f", dev.availableSampleRates[i]);
            }
            fprintf(stdout, "\n");
        }

        if (!dev.availableBufferSizes.empty()) {
            fprintf(stdout, "      buffers: ");
            for (size_t i = 0; i < dev.availableBufferSizes.size(); ++i) {
                if (i > 0) fprintf(stdout, ", ");
                fprintf(stdout, "%d", dev.availableBufferSizes[i]);
            }
            fprintf(stdout, "\n");
        }
        fprintf(stdout, "\n");
    }

    if (!current.name.empty()) {
        fprintf(stdout, "  Active: %s @ %.0f Hz, buffer %d\n\n",
                current.name.c_str(), current.activeSampleRate,
                current.activeBufferSize);
    }
}

int main(int argc, char* argv[]) {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif
    // ── Version flag — must exit before JUCE init (no COM, no audio) ─────────
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "-v") == 0 || std::strcmp(argv[i], "-V") == 0) {
            fprintf(stdout, "supersonic %s (scsynth 3.14.1)\n", VERSION);
            return 0;
        }
        if (std::strcmp(argv[i], "--help") == 0 || std::strcmp(argv[i], "-h") == 0) {
            fprintf(stdout,
                "SuperSonic — scsynth-compatible audio server\n\n"
                "Usage: supersonic [options]\n\n"
                "  -u <port>    UDP port (default: 57110)\n"
                "  -S <rate>    Sample rate (default: 48000)\n"
                "  -Z <size>    Hardware buffer size (default: auto)\n"
                "  -z <size>    scsynth control block size, 32-1024 (default: 128)\n"
                "  -i <num>     Input channels (default: device max; 0 = disable)\n"
                "  -o <num>     Output channels (default: device max)\n"
                "  -n <num>     Max nodes (default: 1024)\n"
                "  -b <num>     Sample buffers (default: 1024)\n"
                "  -a <num>     Audio bus channels (default: 1024)\n"
                "  -c <num>     Control bus channels (default: 16384)\n"
                "  -m <size>    Real-time memory in KB (default: 8192)\n"
                "  -w <num>     Max wire buffers (default: 64)\n"
                "  -B <addr>    Bind address (default: all interfaces)\n"
                "  -H <words>   Audio device (fuzzy match on 'Driver : Device')\n"
                "  -v           Print version and exit\n"
                "  --default-bpm <n>  Opening session tempo (default 120)\n"
                "  --piano-wavetable <path>  MdaPiano sample table (raw int16)\n"
                "  --list-devices     List audio devices and exit\n"
                "\n"
                "Command transports (pick at most one; it replaces the UDP command\n"
                "port — the cue server and outbound OSC are unaffected, and -u still\n"
                "numbers the server's SHM segment; -u 0 disables SHM):\n"
                "  --tcp <port>        TCP, length-prefixed OSC (respects -B)\n"
                "  --uds <path>        Unix socket, stream (macOS/Linux; file 0600)\n"
                "  --uds-dgram <path>  Unix socket, datagram (macOS/Linux; file 0600)\n"
                "  --pipe <name>       Named pipe (Windows; owner-only DACL)\n"
                "  --shm-commands      SHM segment's peer command plane (one trusted\n"
                "                      co-located peer; requires -u > 0)\n"
                "  --max-connections <n>  Stream/pipe connection cap (default 4)\n"
                "\n"
                "  --headless   No audio device; timer-driven render (CI/tests)\n\n"
            );
            return 0;
        }
    }

    juce::ScopedJuceInitialiser_GUI libraryInitialiser;

#ifdef __APPLE__
    // Log macOS microphone permission status. DO NOT request access here —
    // supersonic runs as a background helper child of the GUI, and macOS
    // auto-denies access requests from non-foreground processes without
    // showing the prompt. The GUI (Sonic Pi.app) requests access on our
    // behalf; TCC attributes the permission to the responsible process, so
    // we inherit whatever the user granted.
    MicPermission::logDiagnostics();
    std::string micStatus = MicPermission::status();
    if (micStatus == "denied") {
        fprintf(stderr, "[mic-permission] WARNING: mic access DENIED. live_audio will be silent. "
                "Grant access via System Settings > Privacy & Security > Microphone > Sonic Pi\n");
    } else if (micStatus == "notDetermined") {
        fprintf(stderr, "[mic-permission] status notDetermined — GUI should request on our behalf\n");
    }
    fflush(stderr);
#endif

    // ── --list-devices: enumerate and exit ────────────────────────────────────
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--list-devices") == 0) {
            SupersonicEngine engine;
            engine.onDebug = [](const std::string&) {};
            engine.onReply = [](const uint8_t*, uint32_t) {};

            SupersonicEngine::Config cfg;
            cfg.headless = false;
            cfg.udpPort  = 0;
            engine.init(cfg);

            printDeviceList(engine);

            engine.shutdown();
            return 0;
        }
    }

    // ── Parse scsynth-compatible CLI flags ────────────────────────────────────
    // Channel counts default to SupersonicEngine::kAutoChannelCount (-1) via
    // the Config struct — meaning "open the device with all its channels
    // active". The -i / -o CLI flags override with an explicit count.
    SupersonicEngine::Config cfg;
    cfg.sampleRate           = 48000;
    cfg.bufferSize           = 0;
    cfg.udpPort              = 57110;
    // Recover automatically if the device's callback thread wedges (e.g. a
    // DirectSound cursor-poll spin), so the standalone server keeps running.
    cfg.callbackWatchdog     = true;


    // Command transport selection: at most one of these replaces the UDP
    // command port (the cue server + outbound OSC are independent sockets and
    // stay available either way).
    int         tcpPort = 0;
    std::string udsStreamPath, udsDgramPath, pipeName;
    bool        shmCommands = false;
    uint32_t    maxConns = 4;
    bool        headless = false;

    for (int i = 1; i < argc; ++i) {
        const char* arg = argv[i];
        const char* val = nextArg(i, argc, argv);

        // Opening session tempo (BPM). Long flag — the single-char switch below
        // only matches "-x". Seeded at init so the engine opens at this tempo
        // consistently; embedders (Sonic Pi) pass it to boot at their default.
        if (std::strcmp(arg, "--default-bpm") == 0) {
            if (val) { cfg.defaultBpm = std::atof(val); ++i; }
            continue;
        }

        // Path to the MdaPiano sample table (raw int16). Loaded on the boot
        // thread; if absent, :piano plays silence.
        if (std::strcmp(arg, "--piano-wavetable") == 0) {
            if (val) { cfg.pianoWavetablePath = val; ++i; }
            continue;
        }

        if (std::strcmp(arg, "--tcp") == 0) {
            if (val) { tcpPort = std::atoi(val); ++i; }
            continue;
        }
        if (std::strcmp(arg, "--uds") == 0) {
            if (val) { udsStreamPath = val; ++i; }
            continue;
        }
        if (std::strcmp(arg, "--uds-dgram") == 0) {
            if (val) { udsDgramPath = val; ++i; }
            continue;
        }
        if (std::strcmp(arg, "--pipe") == 0) {
            if (val) { pipeName = val; ++i; }
            continue;
        }
        if (std::strcmp(arg, "--shm-commands") == 0) {
            shmCommands = true;
            continue;
        }
        if (std::strcmp(arg, "--max-connections") == 0) {
            if (val) {
                // Clamp: atoi of a negative/huge value would wrap to a giant
                // uint32; the named-pipe backend pre-spawns one thread per
                // connection slot, so an unclamped value is a thread bomb.
                const long n = std::atol(val);
                maxConns = static_cast<uint32_t>(n < 1 ? 1 : (n > 1024 ? 1024 : n));
                ++i;
            }
            continue;
        }
        // No audio device: the HeadlessDriver renders on a timer thread, so
        // OSC still drains and replies flow — for CI and the transport harness.
        if (std::strcmp(arg, "--headless") == 0) {
            headless = true;
            continue;
        }

        if (arg[0] == '-' && arg[1] != '\0' && arg[2] == '\0' && val) {
            switch (arg[1]) {
            case 'u': cfg.udpPort              = std::atoi(val); ++i; break;
            case 'a': cfg.numAudioBusChannels  = std::atoi(val); ++i; break;
            case 'i': cfg.numInputChannels     = std::atoi(val); ++i; break;
            case 'o': cfg.numOutputChannels    = std::atoi(val); ++i; break;
            case 'b': cfg.numBuffers           = std::atoi(val); ++i; break;
            case 'c': cfg.numControlBusChannels = std::atoi(val); ++i; break;
            case 'm': cfg.realTimeMemorySize   = std::atoi(val); ++i; break;
            case 'B': cfg.bindAddress          = val;            ++i; break;
            case 'S': cfg.sampleRate           = std::atoi(val); ++i; break;
            case 'Z': cfg.bufferSize           = std::atoi(val); ++i; break;
            case 'n': cfg.maxNodes             = std::atoi(val); ++i; break;
            case 'w': cfg.maxWireBufs          = std::atoi(val); ++i; break;
            case 'z': cfg.blockSize            = std::atoi(val); ++i; break;
            case 'H': cfg.hardwareDevice = val; ++i; break;

            // Accepted for scsynth compatibility (ignored):
            case 'U': case 'D': case 'R': case 'l':
            case 'd': case 'r': case 'I': case 'O':
                ++i; break;
            default:
                fprintf(stderr, "[supersonic] unknown flag: %s\n", arg);
                ++i;
                break;
            }
        }
    }

    // Preserve the user's originally-requested input channel count.
    // On macOS this may be zeroed below by the mic-permission guard; on
    // other platforms the value stays equal to cfg.numInputChannels and
    // the later setConfiguredInputChannels call is a harmless no-op.
    int desiredInputChannels = cfg.numInputChannels;

#ifdef __APPLE__
    // If mic permission isn't explicitly authorized, force numInputChannels=0
    // for the boot config. AUHAL's AudioUnitInitialize blocks indefinitely
    // when it tries to open an input stream while TCC permission is pending
    // (the prompt can't be shown from a background helper). Booting output-
    // only avoids the hang; the user can enable inputs later via
    // /supersonic/inputs/enable once permission is granted.
    //
    // Use != 0 rather than > 0 — the new auto-max sentinel is -1, which also
    // represents "the user wants inputs" and must still be disabled pending
    // mic permission.
    if (micStatus != "authorized" && cfg.numInputChannels != 0) {
        fprintf(stderr, "[main] mic status='%s' — forcing numInputChannels=0 "
                "(user can enable inputs later after granting permission)\n",
                micStatus.c_str());
        fflush(stderr);
        cfg.numInputChannels = 0;
    }
#endif

    std::signal(SIGINT,  signalHandler);
    std::signal(SIGTERM, signalHandler);
    std::signal(SIGSEGV, crashHandler);
#ifndef _WIN32
    std::signal(SIGBUS,  crashHandler);
#endif
    std::signal(SIGABRT, crashHandler);
    std::signal(SIGFPE,  crashHandler);

    // At most one command transport may replace the UDP command port.
    const int altTransports = (tcpPort > 0 ? 1 : 0) + (udsStreamPath.empty() ? 0 : 1)
                            + (udsDgramPath.empty() ? 0 : 1) + (pipeName.empty() ? 0 : 1)
                            + (shmCommands ? 1 : 0);
    if (altTransports > 1) {
        fprintf(stderr, "[supersonic] ERROR: pick at most one of --tcp / --uds / "
                        "--uds-dgram / --pipe / --shm-commands\n");
        return 1;
    }
    if (shmCommands && cfg.udpPort <= 0) {
        fprintf(stderr, "[supersonic] ERROR: --shm-commands needs -u > 0 "
                        "(the port names the SHM segment)\n");
        return 1;
    }
    cfg.headless = headless;
    cfg.shmCommands = shmCommands;

    // ── Engine + transport ─────────────────────────────────────────────────────
    // The standalone server owns the command transport; the engine owns no
    // socket. UDP is the default; --tcp/--uds/--uds-dgram/--pipe swap in a
    // connection-oriented or kernel-ACL'd alternative, leaving the UDP command
    // port unbound (the cue server + outbound OSC are separate sockets and are
    // unaffected). Declared before `engine` so they outlive its teardown. Wired
    // as ingress (recv → engine.ingest) and egress (engine → socket).
    UdpOscTransport      udpServer;
    StreamOscTransport   streamServer;
    UdsDgramOscTransport udsDgramServer;
    ShmTransport         shmServer;
    SupersonicEngine     engine;

    engine.onDebug = [](const std::string& s) {
        // Engine messages often carry their own trailing newline (scsynth
        // print conventions) — trim so the log doesn't get blank lines.
        size_t end = s.find_last_not_of("\r\n");
        if (end == std::string::npos) return;  // whitespace-only message
        fprintf(stderr, "[synth] %.*s\n", static_cast<int>(end + 1), s.c_str());
        fflush(stderr);
    };

    auto ingest = [&engine](const uint8_t* d, uint32_t n, uint32_t token) {
        engine.ingest(d, n, token);
    };

    // Select + configure the command transport; started after engine.init()
    // (the engine's rings must exist before the first packet can be ingested).
    IOscTransport* transport = &udpServer;
    std::function<bool()> startTransport;
    std::string transportDesc;
    char descBuf[160];

    if (altTransports > 0) {
        if (shmCommands) {
            // Ingest runs inside the engine (the NRT gateway drains the plane's
            // command ring directly), so unlike the socket transports there is
            // no recv wiring here — only the reply side. The plane exists once
            // engine.init() has created the segment; bind then.
            transport = &shmServer;
            startTransport = [&] {
                shmServer.bindPlaneSlot(engine.peerPlaneSlot());
                return shmServer.ready();
            };
            snprintf(descBuf, sizeof(descBuf), "SHM command plane (segment SuperSonic_%d)",
                     cfg.udpPort);
        } else if (!udsDgramPath.empty()) {
            udsDgramServer.setIngest(ingest);
            udsDgramServer.initialise(udsDgramPath);
            transport = &udsDgramServer;
            startTransport = [&] { return udsDgramServer.start(); };
            snprintf(descBuf, sizeof(descBuf), "UDS dgram socket %s", udsDgramPath.c_str());
        } else {
            streamServer.setIngest(ingest);
            streamServer.setMaxConnections(maxConns);
            if (tcpPort > 0) {
                streamServer.initialiseTcp(tcpPort, cfg.bindAddress);
                snprintf(descBuf, sizeof(descBuf), "TCP port %d (max %u connections)",
                         tcpPort, maxConns);
            } else if (!udsStreamPath.empty()) {
                streamServer.initialiseUds(udsStreamPath);
                snprintf(descBuf, sizeof(descBuf), "UDS stream socket %s (max %u connections)",
                         udsStreamPath.c_str(), maxConns);
            } else {
                streamServer.initialisePipe(pipeName);
                snprintf(descBuf, sizeof(descBuf), "named pipe %s (max %u connections)",
                         pipeName.c_str(), maxConns);
            }
            transport = &streamServer;
            startTransport = [&] { return streamServer.start(); };
        }
        transportDesc = descBuf;
    } else {
        udpServer.setIngest(ingest);
        udpServer.initialise(cfg.udpPort, cfg.bindAddress);
        // UDP stays forgiving (scsynth-compatible): a bind failure is logged
        // but doesn't kill the server.
        startTransport = [&] {
            if (cfg.udpPort > 0) udpServer.start();
            return true;
        };
        snprintf(descBuf, sizeof(descBuf), "UDP port %d", cfg.udpPort);
        transportDesc = descBuf;
    }
    engine.setTransport(transport);

    try {
        engine.init(cfg);
    } catch (const std::exception& e) {
        fprintf(stderr, "[supersonic] ERROR: %s\n", e.what());
        return 1;
    } catch (...) {
        fprintf(stderr, "[supersonic] ERROR: unknown exception during init\n");
        return 1;
    }

    // Start the command transport now that the engine's rings exist for it to
    // feed. Unlike UDP, an alternative transport that can't bind is fatal —
    // the caller chose it for its guarantees and must not get a deaf server.
    if (!startTransport()) {
        fprintf(stderr, "[supersonic] ERROR: command transport failed to start (%s)\n",
                transportDesc.c_str());
        engine.shutdown();
        return 1;
    }

    // Preserve the user's originally-requested input channel count across
    // the permission-pending zeroing above. Otherwise re-enable paths fall
    // back to the hard-coded default of 2, which can produce ghost channels
    // on a mono mic (peak overload, activeIn=2 on a 1-channel device).
    // The value may be -1 (auto-max) — enableInputChannels resolves that to
    // kRequestMaxChannels so JUCE clamps to the device's real input count.
    if (desiredInputChannels != 0 && desiredInputChannels != cfg.numInputChannels) {
        engine.setConfiguredInputChannels(desiredInputChannels);
    }

    auto dev = engine.currentDevice();
    printBanner(VERSION, dev, transportDesc);

    // On macOS, pump the CFRunLoop so AUHAL audio callbacks fire — and so
    // GameController discovery delivers: the gamepad subsystem's controller
    // list only populates while the MAIN run loop is pumped (see
    // rust/supersonic-gamepad/src/gc.rs; hosts without a pump, e.g. a BEAM/NIF
    // embedding, get an empty list).
    // On other platforms, just block.
#ifdef __APPLE__
    // If the run loop is suppressed (aggregate was created at boot),
    // wait for Spider to finish initialising before pumping.
    if (engine.isRunLoopSuppressed()) {
        fprintf(stderr, "[supersonic] waiting for boot to settle before pumping CFRunLoop...\n");
        std::this_thread::sleep_for(std::chrono::seconds(5));
        engine.setRunLoopSuppressed(false);
        fprintf(stderr, "[supersonic] CFRunLoop pump started\n");
    }
    // If we booted with inputs disabled because mic permission was pending,
    // watch for the grant and auto-enable inputs. Re-check once per second
    // until either we're running with inputs or the process shuts down.
    bool needsInputEnableCheck = (cfg.numInputChannels == 0 && micStatus != "authorized");
    int inputCheckCounter = 0;
    while (!gShutdownRequested.load()) {
        if (engine.isRunLoopSuppressed())
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        else
            CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.1, false);

        if (needsInputEnableCheck && ++inputCheckCounter >= 10) {
            inputCheckCounter = 0;
            if (MicPermission::status() == "authorized") {
                fprintf(stderr, "[mic-permission] status now authorized — enabling inputs\n");
                fflush(stderr);
                engine.enableInputChannels(-1);
                needsInputEnableCheck = false;
            }
        }
    }
#elif defined(_WIN32)
    // Pump Win32 messages on the main thread so JUCE's hidden message
    // windows can dispatch what they receive:
    //
    //   * DeviceChangeDetector — listens for WM_DEVICECHANGE on USB
    //     hot-plug, which triggers WASAPIAudioIODeviceType::scan() →
    //     ChangeBroadcaster → AudioDeviceManager → our
    //     changeListenerCallback. This is the path that refreshes the
    //     GUI's audio device dropdowns when devices are added/removed.
    //   * MessageManager::callAsync — posts a custom WM message to
    //     JUCE's hidden window; without a pump those callbacks never run.
    //
    // Without this loop the messages queue forever and JUCE's whole
    // device-refresh path is dead. juce::MessageManager::runDispatchLoop
    // would do the same job but is gated by JUCE_MODAL_LOOPS_PERMITTED
    // (intentionally off for non-GUI apps), so we drive the pump
    // directly. PeekMessage with hWnd=nullptr retrieves both window
    // messages and thread messages owned by this thread, which covers
    // all JUCE hidden windows and queued thread messages.
    MSG msg;
    while (!gShutdownRequested.load()) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        // Sleep keeps shutdown latency to ~50 ms while avoiding a busy
        // spin. MsgWaitForMultipleObjects would let us block precisely
        // on "message arrives or shutdown signalled", but the signal
        // path here is a plain atomic — a coarse sleep is simpler and
        // costs nothing meaningful at this granularity.
        Sleep(50);
    }
#else  // Linux
    while (!gShutdownRequested.load())
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
#endif

    fprintf(stderr, "\n  shutting down...\n");
    // Stop UDP recv before the engine frees its rings — the recv thread feeds
    // engine.ingest(), which writes the IN ring that shutdown() tears down.
    udpServer.stop();
    engine.shutdown();
    return 0;
}
