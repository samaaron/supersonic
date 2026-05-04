/*
 * Main.cpp — SuperSonic server entry point (scsynth drop-in replacement)
 *
 * Accepts scsynth command-line flags so Sonic Pi (and other SC clients)
 * can launch this binary exactly like scsynth.exe.
 *
 * JUCE provides the audio driver (WASAPI/CoreAudio/ALSA),
 * SuperSonic's scsynth core handles synthesis, UDP/OSC for communication.
 */
#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_core/juce_core.h>
#include "SupersonicEngine.h"
#include "supersonic_config.h"

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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

static void printBanner(const char* version, const CurrentDeviceInfo& dev, int udpPort) {
    fprintf(stderr,
        "\n"
        "  ░█▀▀░█░█░█▀█░█▀▀░█▀▄░█▀▀░█▀█░█▀█░▀█▀░█▀▀\n"
        "  ░▀▀█░█░█░█▀▀░█▀▀░█▀▄░▀▀█░█░█░█░█░░█░░█░░\n"
        "  ░▀▀▀░▀▀▀░▀░░░▀▀▀░▀░▀░▀▀▀░▀▀▀░▀░▀░▀▀▀░▀▀▀\n"
        "\n"
        "  SuperSonic v%s (scsynth 3.14.1)\n"
        "\n", version);

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
            "  %d Hz | buffer %d | out %s | in %s\n",
            dev.name.c_str(), dev.typeName.c_str(),
            static_cast<int>(dev.activeSampleRate), dev.activeBufferSize,
            outStr, inStr);
    } else {
        fprintf(stderr, "  headless (no audio device)\n");
    }

    fprintf(stderr, "  UDP port %d\n\n", udpPort);
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
                "  -Z <size>    Buffer size (default: auto)\n"
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
                "  --list-devices     List audio devices and exit\n\n"
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


    for (int i = 1; i < argc; ++i) {
        const char* arg = argv[i];
        const char* val = nextArg(i, argc, argv);

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
            case 'z': cfg.bufferSize           = std::atoi(val); ++i; break;
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

    // ── Engine ────────────────────────────────────────────────────────────────
    SupersonicEngine engine;

    engine.onDebug = [](const std::string& s) {
        fprintf(stderr, "  [scsynth] %s\n", s.c_str());
        fflush(stderr);
    };
    engine.onReply = [](const uint8_t*, uint32_t) {};

    try {
        engine.init(cfg);
    } catch (const std::exception& e) {
        fprintf(stderr, "[supersonic] ERROR: %s\n", e.what());
        return 1;
    } catch (...) {
        fprintf(stderr, "[supersonic] ERROR: unknown exception during init\n");
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
    printBanner(VERSION, dev, cfg.udpPort);

    // On macOS, pump the CFRunLoop so AUHAL audio callbacks fire.
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
    engine.shutdown();
    return 0;
}
