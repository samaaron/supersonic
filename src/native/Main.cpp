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
#include <juce_events/juce_events.h>
#include "SupersonicEngine.h"
#include "supersonic_config.h"

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#ifdef _WIN32
#include <windows.h>
#endif

static constexpr const char* VERSION = SUPERSONIC_VERSION_STRING;

static std::atomic<bool> gShutdownRequested{false};

static void signalHandler(int sig) {
    (void)sig;
    gShutdownRequested.store(true);
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
        fprintf(stderr,
            "  %s (%s)\n"
            "  %d Hz | buffer %d | out %d | in %d\n",
            dev.name.c_str(), dev.typeName.c_str(),
            static_cast<int>(dev.activeSampleRate), dev.activeBufferSize,
            dev.activeOutputChannels, dev.activeInputChannels);
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
                "  -i <num>     Input channels (default: 2)\n"
                "  -o <num>     Output channels (default: 2)\n"
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

    // ── --list-devices: enumerate and exit ────────────────────────────────────
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--list-devices") == 0) {
            SupersonicEngine engine;
            engine.onDebug = [](const std::string&) {};
            engine.onReply = [](const uint8_t*, uint32_t) {};

            SupersonicEngine::Config cfg;
            cfg.headless = false;
            cfg.udpPort  = 0;
            engine.initialise(cfg);

            printDeviceList(engine);

            engine.shutdown();
            return 0;
        }
    }

    // ── Parse scsynth-compatible CLI flags ────────────────────────────────────
    SupersonicEngine::Config cfg;
    cfg.numOutputChannels    = 2;
    cfg.numInputChannels     = 2;
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

    std::signal(SIGINT,  signalHandler);
    std::signal(SIGTERM, signalHandler);

    // ── Engine ────────────────────────────────────────────────────────────────
    SupersonicEngine engine;

    engine.onDebug = [](const std::string& s) {
        fprintf(stderr, "  [scsynth] %s\n", s.c_str());
        fflush(stderr);
    };
    engine.onReply = [](const uint8_t*, uint32_t) {};

    try {
        engine.initialise(cfg);
    } catch (const std::exception& e) {
        fprintf(stderr, "[supersonic] ERROR: %s\n", e.what());
        return 1;
    } catch (...) {
        fprintf(stderr, "[supersonic] ERROR: unknown exception during initialise\n");
        return 1;
    }

    auto dev = engine.currentDevice();
    printBanner(VERSION, dev, cfg.udpPort);

    // Pump the JUCE message loop until Ctrl+C / SIGTERM.
    // CoreAudio property listeners and AirPlay negotiation dispatch through
    // CFRunLoop — if we just block with WaitableEvent::wait() the run loop
    // never pumps and AirPlay device switches hang indefinitely.
    //
    // A timer checks the shutdown flag and calls stopDispatchLoop() from
    // the message thread (signal handlers can't safely call JUCE methods).
    struct ShutdownPoller : public juce::Timer {
        void timerCallback() override {
            if (gShutdownRequested.load())
                juce::MessageManager::getInstance()->stopDispatchLoop();
        }
    } shutdownPoller;
    shutdownPoller.startTimer(100);

    juce::MessageManager::getInstance()->runDispatchLoop();

    fprintf(stderr, "\n  shutting down...\n");
    engine.shutdown();
    return 0;
}
