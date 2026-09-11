// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2025-2026 Sam Aaron
// See EngineHost.h. Brought over from clockwork's former reference host, which is gone:
// this is SuperSonic's process, built on clockwork the library.
#include "EngineHost.h"

#include "DevicePolicy.h"
#include "EgressRouter.h"
#include "clockwork_client.h"
#include "clockwork_product.h"
#include "native/LinkAudioBridge.h"
#ifdef __APPLE__
#  include "cocoa_event_pump.h"
#  include "MicPermission.h"
#  include <CoreFoundation/CoreFoundation.h>
#endif

#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#ifdef _WIN32
#  include <windows.h>
#else
#  include <execinfo.h>
#  include <unistd.h>
#endif

namespace supersonic_host {

// ── The command line ─────────────────────────────────────────────────────────

std::string usage(const char* productName) {
    std::string p = productName ? productName : "clockwork";
    return
        p + " — audio runtime\n\n"
        "Usage: " + p + " [options]\n\n"
        "  -u <port>    UDP port (default: 57110)\n"
        "  -S <rate>    Sample rate (default: 48000)\n"
        "  -Z <size>    Hardware buffer size (default: auto)\n"
        "  -z <size>    DSP control block size, 32-1024 (default: 128)\n"
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
        "  --app-name <name>  Name published to OS audio/MIDI registries\n"
        "                     (PipeWire, ALSA seq, Link; default: " + p + ")\n"
        "  --audio-driver <name>  Driver to boot on (e.g. CoreAudio, ASIO, PipeWire)\n"
        "  --list-devices     List audio devices and exit\n"
        "\n"
        "Command transports (pick at most one; it replaces the UDP command\n"
        "port — the cue server and outbound OSC are unaffected; -u > 0 is\n"
        "what creates the SHM segment, and -u 0 disables it):\n"
        "  --tcp <port>        TCP, length-prefixed OSC (respects -B)\n"
        "  --uds <path>        Unix socket, stream (macOS/Linux; file 0600)\n"
        "  --uds-dgram <path>  Unix socket, datagram (macOS/Linux; file 0600)\n"
        "  --pipe <name>       Named pipe (Windows; owner-only DACL)\n"
        "  --shm-commands      SHM segment's peer command plane (one trusted\n"
        "                      co-located peer; requires -u > 0)\n"
        "  --max-connections <n>  Stream/pipe connection cap (default 4)\n"
        "  --inbox-mb <n>      The inbox lane a client loads samples and other\n"
        "                      assets into, in MB (default 512). Address space,\n"
        "                      not memory: pages are committed as they are written.\n"
        "\n"
        "Shared memory (needs -u > 0). The segment is anonymous; readers get\n"
        "it from the attach endpoint, a Unix socket (macOS/Linux) or named\n"
        "pipe (Windows) serving this user only:\n"
        "  --shm-endpoint <path|pipe>  Where to serve it (default: derived\n"
        "                      from -u under XDG_RUNTIME_DIR or TMPDIR, or\n"
        "                      \\\\.\\pipe\\clockwork-shm-<port>)\n"
        "\n"
        "  --headless   No audio device; timer-driven render (CI/tests)\n\n";
}

static const char* nextArg(int i, int argc, char* const argv[]) {
    return (i + 1 < argc) ? argv[i + 1] : nullptr;
}

bool parseArgs(int argc, char* const argv[], Options& o, std::string* err) {
    ClockworkEngine::Config& cfg = o.cfg;
    cfg.sampleRate       = 48000;
    cfg.bufferSize       = 0;
    cfg.udpPort          = 57110;
    // Recover automatically if the device's callback thread wedges (e.g. a
    // DirectSound cursor-poll spin), so a standalone process keeps running.
    cfg.callbackWatchdog = true;
    long inboxMb = 512;   // the host's default: generous, because it is only address space

    for (int i = 1; i < argc; ++i) {
        const char* arg = argv[i];
        const char* val = nextArg(i, argc, argv);

        if (std::strcmp(arg, "--default-bpm") == 0) { if (val) { cfg.defaultBpm = std::atof(val); ++i; } continue; }
        // Name published to OS audio/MIDI registries (PipeWire nodes, ALSA seq
        // MIDI clients, macOS aggregate devices, Link peers).
        if (std::strcmp(arg, "--app-name") == 0) { if (val && *val) { cfg.appName = val; ++i; } continue; }
        // Driver (JUCE device type) to boot on; resolution rules live in resolveBootDriver.
        if (std::strcmp(arg, "--audio-driver") == 0) { if (val && *val) { cfg.audioDriver = val; ++i; } continue; }
        if (std::strcmp(arg, "--tcp") == 0)          { if (val) { o.tcpPort = std::atoi(val); ++i; } continue; }
        if (std::strcmp(arg, "--uds") == 0)          { if (val) { o.udsStreamPath = val; ++i; } continue; }
        if (std::strcmp(arg, "--uds-dgram") == 0)    { if (val) { o.udsDgramPath = val; ++i; } continue; }
        if (std::strcmp(arg, "--pipe") == 0)         { if (val) { o.pipeName = val; ++i; } continue; }
        if (std::strcmp(arg, "--shm-commands") == 0) { o.shmCommands = true; continue; }
        if (std::strcmp(arg, "--shm-endpoint") == 0) { if (val) { o.shmEndpoint = val; ++i; } continue; }
        if (std::strcmp(arg, "--inbox-mb") == 0) {
            if (val) {
                // Header offsets are 32-bit: the lane and everything before
                // it must stay under 4 GB.
                const long n = std::atol(val);
                inboxMb = n < 1 ? 1 : (n > 3072 ? 3072 : n);
                ++i;
            }
            continue;
        }
        if (std::strcmp(arg, "--max-connections") == 0) {
            if (val) {
                // Clamp: the named-pipe backend pre-spawns one thread per
                // connection slot, so an unclamped value is a thread bomb.
                const long n = std::atol(val);
                o.maxConnections = static_cast<uint32_t>(n < 1 ? 1 : (n > 1024 ? 1024 : n));
                ++i;
            }
            continue;
        }
        // No audio device: the HeadlessDriver renders on a timer thread, so
        // OSC still drains and replies flow — for CI and the transport harness.
        if (std::strcmp(arg, "--headless") == 0) { o.headless = true; continue; }

        // Every known long flag continues above, so a "--" arg reaching here
        // is unknown. Reported rather than skipped: a dropped option reads as
        // configuration applied when it wasn't.
        if (arg[0] == '-' && arg[1] == '-') {
            o.warnings.push_back(std::string("unknown flag: ") + arg);
            continue;
        }

        if (arg[0] == '-' && arg[1] != '\0' && arg[2] == '\0' && val) {
            switch (arg[1]) {
            case 'u': cfg.udpPort               = std::atoi(val); ++i; break;
            case 'a': cfg.numAudioBusChannels   = std::atoi(val); ++i; break;
            case 'i': cfg.numInputChannels      = std::atoi(val); ++i; break;
            case 'o': cfg.numOutputChannels     = std::atoi(val); ++i; break;
            case 'b': cfg.numBuffers            = std::atoi(val); ++i; break;
            case 'c': cfg.numControlBusChannels = std::atoi(val); ++i; break;
            case 'm': cfg.realTimeMemorySize    = std::atoi(val); ++i; break;
            case 'B': cfg.bindAddress           = val;            ++i; break;
            case 'S': cfg.sampleRate            = std::atoi(val); ++i; break;
            case 'Z': cfg.bufferSize            = std::atoi(val); ++i; break;
            case 'n': cfg.maxNodes              = std::atoi(val); ++i; break;
            case 'w': cfg.maxWireBufs           = std::atoi(val); ++i; break;
            case 'z': cfg.blockSize             = std::atoi(val); ++i; break;
            case 'H': {
                // scsynth's -H: "<in> <out>", or a single name for both.
                auto req = clockwork::device::parseHardwareFlag(val, nextArg(i + 1, argc, argv));
                cfg.hardwareDevice = req.outputDevice;
                cfg.inputDevice    = req.inputDevice;
                i += req.secondTokenUsed ? 2 : 1;
                break;
            }
            // Accepted for scsynth compatibility (ignored):
            case 'U': case 'D': case 'R': case 'l':
            case 'd': case 'r': case 'I': case 'O':
                ++i; break;
            default:
                o.warnings.push_back(std::string("unknown flag: ") + arg);
                ++i;
                break;
            }
        }
    }

    o.desiredInputChannels = cfg.numInputChannels;

    const int alternatives = (o.tcpPort > 0 ? 1 : 0) + (o.udsStreamPath.empty() ? 0 : 1)
                           + (o.udsDgramPath.empty() ? 0 : 1) + (o.pipeName.empty() ? 0 : 1)
                           + (o.shmCommands ? 1 : 0);
    if (alternatives > 1) {
        if (err) *err = "pick at most one of --tcp / --uds / --uds-dgram / --pipe / --shm-commands";
        return false;
    }
    if (o.shmCommands && cfg.udpPort <= 0) {
        if (err) *err = "--shm-commands needs -u > 0 (-u > 0 is what creates the SHM segment)";
        return false;
    }
    if (!o.shmEndpoint.empty() && cfg.udpPort <= 0) {
        if (err) *err = "--shm-endpoint needs -u > 0 (-u > 0 is what creates the SHM segment)";
        return false;
    }
    if (cfg.udpPort > 0 && o.shmEndpoint.empty())
        o.shmEndpoint = shm_attach::default_endpoint(static_cast<unsigned>(cfg.udpPort));
    cfg.headless    = o.headless;
    cfg.shmCommands = o.shmCommands;
    cfg.inboxBytes  = static_cast<size_t>(inboxMb) * 1024u * 1024u;
    return true;
}

// ── The command transports ───────────────────────────────────────────────────

IOscTransport* CommandTransports::select(const Options& o, std::atomic<ShmPeerPlaneHeader*>* peerPlaneSlot,
                                         Ingest ingest, std::string* err) {
    mIngest = std::move(ingest);
    const ClockworkEngine::Config& cfg = o.cfg;
    char desc[160];

    if (o.shmCommands) {
        // Ingest runs inside the engine (the NRT gateway drains the plane's
        // command ring directly), so there is no recv wiring here — only the
        // reply side. The plane exists once engine.init() has created the
        // segment; bind then.
        if (!peerPlaneSlot) { if (err) *err = "--shm-commands needs an engine's peer plane"; return nullptr; }
        mStart = [this, peerPlaneSlot] { mShm.bindPlaneSlot(peerPlaneSlot); return mShm.ready(); };
        snprintf(desc, sizeof desc, "SHM command plane (segment via %s)", o.shmEndpoint.c_str());
        mDesc = desc;
        return &mShm;
    }
    if (!o.udsDgramPath.empty()) {
        mUdsDgram.setIngest(mIngest);
        mUdsDgram.initialise(o.udsDgramPath);
        mStart = [this] { return mUdsDgram.start(); };
        snprintf(desc, sizeof desc, "UDS dgram socket %s", o.udsDgramPath.c_str());
        mDesc = desc;
        return &mUdsDgram;
    }
    if (o.tcpPort > 0 || !o.udsStreamPath.empty() || !o.pipeName.empty()) {
        mStream.setIngest(mIngest);
        mStream.setMaxConnections(o.maxConnections);
        if (o.tcpPort > 0) {
            mStream.initialiseTcp(o.tcpPort, cfg.bindAddress);
            snprintf(desc, sizeof desc, "TCP port %d (max %u connections)", o.tcpPort, o.maxConnections);
        } else if (!o.udsStreamPath.empty()) {
            mStream.initialiseUds(o.udsStreamPath);
            snprintf(desc, sizeof desc, "UDS stream socket %s (max %u connections)", o.udsStreamPath.c_str(), o.maxConnections);
        } else {
            mStream.initialisePipe(o.pipeName);
            snprintf(desc, sizeof desc, "named pipe %s (max %u connections)", o.pipeName.c_str(), o.maxConnections);
        }
        mStart = [this] { return mStream.start(); };
        mDesc = desc;
        return &mStream;
    }

    // UDP, the default. Bound and receiving NOW, before the engine boots, so
    // nothing a client sends during a slow device open (an ASIO open can take
    // >10 s) bounces as ICMP port-unreachable and is lost — a client fires its
    // subsystem subscriptions as soon as it boots, and a lost subscribe costs
    // every event that subsystem would ever push. Packets queue here and
    // flush, in arrival order, on start(); one that races the flush waits on
    // the mutex and lands after it. UDP stays forgiving (scsynth-compatible):
    // a bind failure is logged, not fatal.
    mUdp.setIngest([this](const uint8_t* d, uint32_t n, uint32_t token) {
        if (mReady.load(std::memory_order_acquire)) { mIngest(d, n, token); return; }
        std::lock_guard<std::mutex> lk(mPendingMut);
        if (mReady.load(std::memory_order_acquire)) { mIngest(d, n, token); return; }
        if (mPending.size() < kMaxPending) {
            mPending.emplace_back(std::vector<uint8_t>(d, d + n), token);
        } else if (!mDropWarned) {
            mDropWarned = true;
            fprintf(stderr, CLOCKWORK_LOG_PREFIX "boot command queue full — dropping further packets until init completes\n");
            fflush(stderr);
        }
    });
    mUdp.initialise(cfg.udpPort, cfg.bindAddress);
    if (cfg.udpPort > 0) mUdp.start();
    mStart = [this] {
        std::lock_guard<std::mutex> lk(mPendingMut);
        for (auto& p : mPending) mIngest(p.first.data(), static_cast<uint32_t>(p.first.size()), p.second);
        if (!mPending.empty()) {
            fprintf(stderr, CLOCKWORK_LOG_PREFIX "flushed %zu command packet(s) queued during boot\n", mPending.size());
            fflush(stderr);
        }
        mPending.clear();
        mReady.store(true, std::memory_order_release);
        return true;
    };
    snprintf(desc, sizeof desc, "UDP port %d", cfg.udpPort);
    mDesc = desc;
    return &mUdp;
}

bool CommandTransports::start() { return mStart ? mStart() : false; }

void CommandTransports::stop() {
    // Every receiving transport, so no recv thread is left feeding an
    // engine that is coming down. Each stop() is idempotent.
    mUdp.stop();
    mStream.stop();
    mUdsDgram.stop();
}

// ── The egress pump ──────────────────────────────────────────────────────────

EgressPump::EgressPump(ClockworkEngine& engine, IOscTransport& transport,
                       std::function<void(const std::string&)> log)
    : mEngine(engine), mTransport(transport), mLog(std::move(log)) {}

EgressPump::~EgressPump() { stop(); }

void EgressPump::start() {
    if (mThread.joinable()) return;
    mStop.store(false);
    mThread = std::thread([this] { run(); });
}

void EgressPump::stop() {
    mStop.store(true);
    if (mThread.joinable()) mThread.join();
}

void EgressPump::run() {
    // One read, both rings, a batch at a time; the array is ours, which is
    // the ABI's rule. Nothing to take: wait a moment. The audio thread
    // answers every block (2.7 ms at 128/48k), so the wait is well under
    // that, and a reply is on the wire within a block of being framed.
    ClockworkClientMessage batch[64];
    while (!mStop.load(std::memory_order_relaxed)) {
        ClockworkClient* c = mEngine.egressClient();
        if (!c) { std::this_thread::sleep_for(std::chrono::milliseconds(1)); continue; }
        uint32_t taken = 0;
        for (;;) {
            const uint32_t n = clockwork_client_poll(c, batch, sizeof batch / sizeof batch[0]);
            for (uint32_t i = 0; i < n; ++i)
                clockwork_route_egress(mTransport, batch[i].origin, batch[i].route,
                                       batch[i].bytes, batch[i].length, mLog);
            taken += n;
            if (n < sizeof batch / sizeof batch[0]) break;
        }
        if (taken) mRouted.fetch_add(taken, std::memory_order_relaxed);
        else       std::this_thread::sleep_for(std::chrono::microseconds(500));
    }
}

// ── The segment ──────────────────────────────────────────────────────────────

bool serveSegment(shm_attach::server& server, ClockworkEngine& engine, const Options& o,
                  const std::function<void(const std::string&)>& log) {
    if (o.cfg.udpPort <= 0) return false;
    std::string err;
    if (!detail_shm_segment::shm_handle_valid(engine.shmNativeHandle())) {
        log("no shared-memory segment; attach endpoint not started");
        return false;
    }
    if (server.start(o.shmEndpoint, engine.shmNativeHandle(), engine.shmSegmentSize(), &err)) {
        log("shared memory served at " + o.shmEndpoint);
        return true;
    }
    log("ERROR: shared-memory attach endpoint: " + err);
    return false;
}

// ── Signals ──────────────────────────────────────────────────────────────────

static std::atomic<bool> gShutdownRequested { false };
static const char*       gLogPrefix = "[clockwork] ";

bool shutdownRequested() { return gShutdownRequested.load(); }
void requestShutdown()   { gShutdownRequested.store(true); }

static void signalHandler(int) { gShutdownRequested.store(true); }

void installSignalHandlers() {
    std::signal(SIGINT,  signalHandler);
    std::signal(SIGTERM, signalHandler);
}

#ifndef _WIN32
// Async-signal-safe stderr writers. fprintf inside a signal handler can
// take a stdio lock the crashed thread already holds, or allocate — either
// turns a diagnosable fault into a silent death or a hang. write(2) is on
// the async-signal-safe list.
static void crashWrite(const char* s) {
    size_t n = 0;
    while (s[n]) ++n;
    const ssize_t r = write(STDERR_FILENO, s, n);
    (void)r;
}

static void crashWriteUnsigned(uintptr_t v, int base) {
    char buf[2 + sizeof(v) * 2 + 1];
    char* end = buf + sizeof(buf);
    char* p   = end;
    *--p = '\0';
    do {
        *--p = "0123456789abcdef"[v % static_cast<uintptr_t>(base)];
        v /= static_cast<uintptr_t>(base);
    } while (v != 0);
    if (base == 16) { *--p = 'x'; *--p = '0'; }
    crashWrite(p);
}

static void crashHandler(int sig, siginfo_t* info, void*) {
    const char* name = "UNKNOWN";
    switch (sig) {
        case SIGSEGV: name = "SIGSEGV"; break;
        case SIGBUS:  name = "SIGBUS";  break;
        case SIGABRT: name = "SIGABRT"; break;
        case SIGFPE:  name = "SIGFPE";  break;
        case SIGILL:  name = "SIGILL";  break;
        case SIGTRAP: name = "SIGTRAP"; break;
    }
    crashWrite("\n"); crashWrite(gLogPrefix); crashWrite("FATAL: ");
    crashWrite(name);
    crashWrite(" (signal ");
    crashWriteUnsigned(static_cast<uintptr_t>(sig), 10);
    crashWrite(") fault addr=");
    crashWriteUnsigned(info ? reinterpret_cast<uintptr_t>(info->si_addr) : 0, 16);
    crashWrite("\n"); crashWrite(gLogPrefix); crashWrite("Backtrace:\n");
    // backtrace_symbols_fd writes straight to the fd (no malloc). Not
    // formally async-signal-safe, but the essential FATAL line is already
    // out above; from here everything is best-effort bonus detail.
    void* frames[64];
    const int n = backtrace(frames, 64);
    backtrace_symbols_fd(frames, n, STDERR_FILENO);
    crashWrite(gLogPrefix); crashWrite("Exiting due to crash.\n");
    _exit(128 + sig);
}

void installCrashHandlers(const char* logPrefix) {
    if (logPrefix) gLogPrefix = logPrefix;
    // Dedicated signal stack: a stack-overflow SIGSEGV is delivered on the
    // stack that just overflowed, so without an alternate stack the handler
    // faults before its first instruction and the process dies with no
    // output at all. SIGSTKSZ stopped being a constant expression in glibc
    // 2.34, so use a generous fixed block.
    static char altStack[64 * 1024];
    stack_t ss = {};
    ss.ss_sp    = altStack;
    ss.ss_size  = sizeof(altStack);
    ss.ss_flags = 0;
    sigaltstack(&ss, nullptr);

    struct sigaction sa = {};
    sa.sa_sigaction = crashHandler;
    // SA_RESETHAND: a second fault inside the handler takes the default
    // action (die) instead of looping. SIGILL and SIGTRAP are included
    // because arm64 aborts frequently surface as brk/udf instructions.
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_RESETHAND;
    sigemptyset(&sa.sa_mask);
    for (int sig : { SIGSEGV, SIGBUS, SIGABRT, SIGFPE, SIGILL, SIGTRAP })
        sigaction(sig, &sa, nullptr);
}
#else
static void crashHandler(int sig) {
    const char* name = "UNKNOWN";
    if      (sig == SIGSEGV) name = "SIGSEGV";
    else if (sig == SIGABRT) name = "SIGABRT";
    else if (sig == SIGFPE)  name = "SIGFPE";
    else if (sig == SIGILL)  name = "SIGILL";
    fprintf(stderr, "\n%sFATAL: %s (signal %d)\n%sExiting due to crash.\n", gLogPrefix, name, sig, gLogPrefix);
    fflush(stderr);
    _exit(128 + sig);
}

void installCrashHandlers(const char* logPrefix) {
    if (logPrefix) gLogPrefix = logPrefix;
    std::signal(SIGSEGV, crashHandler);
    std::signal(SIGABRT, crashHandler);
    std::signal(SIGFPE,  crashHandler);
    std::signal(SIGILL,  crashHandler);
}
#endif

// ── The run loop ─────────────────────────────────────────────────────────────

void runUntilShutdown(ClockworkEngine& engine, const std::function<void(const std::string&)>& log,
                      const std::function<void()>& tick) {
    int ticks = 0;
    auto maybeTick = [&](int per) { if (tick && ++ticks >= per) { ticks = 0; tick(); } };
#ifdef __APPLE__
    // If the run loop is suppressed (an aggregate was created at boot), wait
    // for the client to finish initialising before pumping.
    if (engine.isRunLoopSuppressed()) {
        log("waiting for boot to settle before pumping CFRunLoop...");
        std::this_thread::sleep_for(std::chrono::seconds(5));
        engine.setRunLoopSuppressed(false);
        log("CFRunLoop pump started");
    }
    while (!gShutdownRequested.load()) {
        if (engine.isRunLoopSuppressed()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        } else {
            ClockworkAutoreleasePool pool;   // drained per turn; see cocoa_event_pump.h
            CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.1, false);
            // The run loop above is enough for a window to appear and redraw,
            // and NOT enough for it to be clicked: AppKit delivers input by
            // dequeuing from NSApplication and calling -sendEvent:.
            clockwork_pump_cocoa_events();
        }
        maybeTick(10);
    }
#elif defined(_WIN32)
    // Pump Win32 messages on the main thread so JUCE's hidden message windows
    // can dispatch what they receive (WM_DEVICECHANGE for hot-plug, and
    // MessageManager::callAsync). juce::MessageManager::runDispatchLoop would
    // do the same job but is gated by JUCE_MODAL_LOOPS_PERMITTED.
    (void)engine; (void)log;
    MSG msg;
    while (!gShutdownRequested.load()) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        Sleep(50);
        maybeTick(20);
    }
#else
    (void)engine; (void)log;
    while (!gShutdownRequested.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        maybeTick(10);
    }
#endif
}

// ── The banner and the device list ───────────────────────────────────────────

void printBanner(const Identity& id, const CurrentDeviceInfo& dev, const std::string& transportDesc) {
    fprintf(stderr, "\n%s\n\n  %s v%s\n\n", id.banner, id.name, id.version);
    fprintf(stderr, "  Compiled:");
#if CLOCKWORK_SYNTH
    fprintf(stderr, " synth");
#endif
#if CLOCKWORK_LINK
    fprintf(stderr, " Link");
#endif
    // CLOCKWORK_LINK_AUDIO is LinkAudioBridge.h's, derived there from
    // CLOCKWORK_LINK + CLOCKWORK_WITH_LINK_AUDIO. Asking it rather than
    // re-deriving the condition here is the point: the banner used to carry its
    // own copy, spelled `CLOCKWORK_LINK && CLOCKWORK_SYNTH`, and kept claiming
    // Link-Audio for years after the bridge was corrected away from it.
#if CLOCKWORK_LINK_AUDIO
    fprintf(stderr, " Link-Audio");
#endif
#if CLOCKWORK_MIDI
    fprintf(stderr, " MIDI");
#endif
#if CLOCKWORK_GAMEPAD
    fprintf(stderr, " Gamepad");
#endif
    fprintf(stderr, "\n");

    if (!dev.name.empty()) {
        // "out A/M in B/N" — A channels currently routed / M available on the
        // device (likewise for inputs).
        char outStr[32], inStr[32];
        if (dev.maxOutputChannels > 0 && dev.maxOutputChannels != dev.activeOutputChannels)
            snprintf(outStr, sizeof(outStr), "%d/%d", dev.activeOutputChannels, dev.maxOutputChannels);
        else
            snprintf(outStr, sizeof(outStr), "%d", dev.activeOutputChannels);
        if (dev.maxInputChannels > 0 && dev.maxInputChannels != dev.activeInputChannels)
            snprintf(inStr, sizeof(inStr), "%d/%d", dev.activeInputChannels, dev.maxInputChannels);
        else
            snprintf(inStr, sizeof(inStr), "%d", dev.activeInputChannels);
        fprintf(stderr, "  %s (%s)\n  %d Hz | block %d | buffer %d | out %s | in %s\n",
                dev.name.c_str(), dev.typeName.c_str(),
                static_cast<int>(dev.activeSampleRate), dev.controlBlockSize, dev.activeBufferSize,
                outStr, inStr);
    } else {
        fprintf(stderr, "  headless (no audio device)\n");
    }
    fprintf(stderr, "  %s\n\n", transportDesc.c_str());
    fflush(stderr);
}

void printDeviceList(ClockworkEngine& engine) {
    auto devices = engine.listDevices();
    auto current = engine.currentDevice();
    fprintf(stdout, "\n  Audio Devices\n  ─────────────\n\n");
    for (auto& dev : devices) {
        if (dev.isWirelessTransport()) continue;
        const bool isCurrent = (dev.name == current.name && dev.typeName == current.typeName);
        fprintf(stdout, "  %s %s : %s\n", isCurrent ? "▸" : " ", dev.typeName.c_str(), dev.name.c_str());
        if (dev.maxOutputChannels > 0 || dev.maxInputChannels > 0)
            fprintf(stdout, "      channels: %d out, %d in\n", dev.maxOutputChannels, dev.maxInputChannels);
        if (!dev.availableSampleRates.empty()) {
            fprintf(stdout, "      rates:   ");
            for (size_t i = 0; i < dev.availableSampleRates.size(); ++i)
                fprintf(stdout, "%s%.0f", i ? ", " : "", dev.availableSampleRates[i]);
            fprintf(stdout, "\n");
        }
        if (!dev.availableBufferSizes.empty()) {
            fprintf(stdout, "      buffers: ");
            for (size_t i = 0; i < dev.availableBufferSizes.size(); ++i)
                fprintf(stdout, "%s%d", i ? ", " : "", dev.availableBufferSizes[i]);
            fprintf(stdout, "\n");
        }
        fprintf(stdout, "\n");
    }
    if (!current.name.empty())
        fprintf(stdout, "  Active: %s @ %.0f Hz, buffer %d\n\n", current.name.c_str(),
                current.activeSampleRate, current.activeBufferSize);
}

#ifdef __APPLE__
std::string micPermissionStatus() { return MicPermission::status(); }

std::string applyMicPermissionGuard(Options& o) {
    // DO NOT request access here — this process typically runs as a
    // background helper child of a GUI, and macOS auto-denies requests from
    // non-foreground processes without showing the prompt. The GUI requests
    // on our behalf; TCC attributes the permission to the responsible
    // process, so we inherit whatever the user granted.
    MicPermission::logDiagnostics();
    const std::string status = MicPermission::status();
    if (status == "denied") {
        fprintf(stderr, "[mic-permission] WARNING: mic access DENIED. live_audio will be silent. "
                "Grant access via System Settings > Privacy & Security > Microphone, "
                "for the app that launched this engine\n");
    } else if (status == "notDetermined") {
        fprintf(stderr, "[mic-permission] status notDetermined — GUI should request on our behalf\n");
    }
    // Unless authorized, boot output-only: AUHAL's AudioUnitInitialize blocks
    // indefinitely opening an input stream while TCC permission is pending.
    // != 0 rather than > 0: the auto-max sentinel is -1, which also means
    // "the user wants inputs".
    if (status != "authorized" && o.cfg.numInputChannels != 0) {
        fprintf(stderr, "[main] mic status='%s' — forcing numInputChannels=0 "
                "(user can enable inputs later after granting permission)\n", status.c_str());
        o.cfg.numInputChannels = 0;
    }
    fflush(stderr);
    return status;
}
#endif

} // namespace supersonic_host
