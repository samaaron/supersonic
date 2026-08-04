/*
 * SupersonicEngine.cpp
 */
#include "SupersonicEngine.h"
#include "supersonic_config.h"   // ss_log
#include "scheduler/MidiClockOut.h"
#include "AggregateDeviceHelper.h"
#include "DevicePolicy.h"
#include "AsioDriverCheck.h"
#include "PipeWireAudio.h"
#include "src/audio_processor.h"
#include "src/lanes/lanes.h"
#include "audio_config.h"
#include "src/shared_memory.h"
#include "src/osc_debug.h"
#include "src/clock_math.h"
#include "osc/OscReceivedElements.h"
#include "osc/OscOutboundPacketStream.h"
#include "RingBufferWriter.h"
#include "src/IngressCallCtx.h"
#include "synth/server/SC_Prototypes.h"  // zfree
#include "RecordWriter.h"
#include <chrono>
#include <cstdarg>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <thread>
#ifdef __linux__
#include <dlfcn.h>
#endif
#ifdef __APPLE__
#include <CoreAudio/CoreAudio.h>
#include "MicPermission.h"
#endif

extern "C" {
    // Publishes NRT control-thread blocking into the native-stats region
    // (SC_World.cpp). Called from the watchdog poll.
    void World_PublishNrtBlocking(uint32_t maxPassUs, uint32_t recentWorstUs,
                                  uint32_t inFlightUs);

    // Global used by init_memory() to pass external shared memory to World_New.
    // Declared extern "C" because init_memory() references it from an extern "C" block.
    void* g_external_shared_memory = nullptr;

    // Unified-arena base. When non-null, init_memory() points the engine's
    // whole shared_memory.h blob (rings, control, metrics, node-tree, audio
    // taps, scope) at this address — the public POSIX segment — instead of the
    // process-local ring_buffer_storage. Everything is observable for free.
    extern uint8_t* g_external_segment;

    // Real-pointer arena base (== init_memory's `shared_memory`): the public
    // segment when present, else ring_buffer_storage. Use for any engine region
    // (world-options, rings, timing) so native glue stays consistent with the
    // unified arena instead of hardcoding ring_buffer_storage.
    void* get_shared_memory_base();

    void destroy_world();
    void rebuild_world(double sample_rate);
}

namespace {
// Arena base for engine-region access in this file. Valid after init_memory()
// (i.e. once the World has booted), which all callers below satisfy.
inline uint8_t* sp_arena() { return static_cast<uint8_t*>(get_shared_memory_base()); }

// Name published to OS registries (PipeWire nodes, ALSA seq MIDI clients,
// macOS aggregate devices, Link peers). Written once in init() from
// cfg.appName before any device manager or MIDI subsystem exists, read-only
// after; consumers declare `extern "C" const char* ss_app_name()`.
std::string sPublishedAppName = "SuperSonic";
}

extern "C" const char* ss_app_name() { return sPublishedAppName.c_str(); }

namespace {
// The device-name identity predicate lives in DevicePolicy
// (sameDeviceName): equal modulo JUCE's " (N)" duplicate suffix, strict.
using sonicpi::device::sameDeviceName;

#ifdef __linux__
// Silence libjack's stderr chatter on boxes where no jackd / pipewire-
// jack server is running. With JUCE_JACK=1 enabled, JUCE's
// JackAudioIODeviceType calls jack_client_open() during scanForDevices;
// libjack writes a two-line "connect(2) ... failed / attempt to connect
// to server failed" pair for every failed attempt. Routing that through
// our noop callback eliminates the spam without affecting the scan's
// outcome (JUCE still sees the open-failed return and correctly reports
// zero devices for the JACK type).
void silentJackLog(const char*) {}

void silenceJackLogsIfPossible() {
    // Only relevant after libjack has been loaded. JUCE dlopens it
    // lazily inside JackAudioIODeviceType's ctor; calling this once
    // after AudioDeviceManager construction catches that first scan.
    // If libjack isn't loadable at all (no package installed), dlopen
    // fails and we skip quietly.
    void* handle = dlopen("libjack.so.0", RTLD_LAZY | RTLD_NOLOAD);
    if (!handle) handle = dlopen("libjack.so.0", RTLD_LAZY);
    if (!handle) return;
    using set_fn = void (*)(void (*)(const char*));
    if (auto set_err  = (set_fn)dlsym(handle, "jack_set_error_function"))
        set_err(silentJackLog);
    if (auto set_info = (set_fn)dlsym(handle, "jack_set_info_function"))
        set_info(silentJackLog);
    // Don't dlclose — JUCE needs libjack resident for its own dlsym path.
}
#endif
}

SupersonicEngine::SupersonicEngine() = default;

void SupersonicEngine::recordSwapPreferences(const std::string& deviceName,
                                             const std::string& inputDeviceName,
                                             double sampleRate,
                                             SwapOrigin origin) {
    if (!deviceName.empty() && sampleRate > 0) {
        // Cap to avoid unbounded growth across long sessions of hot-plug
        // cycling (USB dock / AirPlay / Bluetooth churn can accumulate
        // device-name variants indefinitely). 32 is well above any realistic
        // device set a user juggles in one session — if we hit it, the
        // entries we forget get re-probed next time the device is chosen.
        static constexpr size_t kMaxDeviceRateMemoryEntries = 32;
        if (mDeviceRateMemory.size() >= kMaxDeviceRateMemoryEntries
            && mDeviceRateMemory.find(deviceName) == mDeviceRateMemory.end())
            mDeviceRateMemory.clear();
        mDeviceRateMemory[deviceName] = static_cast<int>(sampleRate);
    }

    // Everything below is user-intent state. Internal swaps (recovery
    // reopen, hotplug re-attach) record only the rate memory above — a
    // recovery landing on the system default must not become the
    // "preferred" device, and must not consume a pending driver intent.
    if (origin != SwapOrigin::User)
        return;

    // Track the user's long-lived preferred device for hot-plug re-attach.
    // An explicit deviceName means the caller picked it (GUI switch, OSC,
    // setDeviceMode); remember it even across cycles where the device
    // disappears. inputDeviceName == "__none__" means "disable inputs"
    // (user intent to not have any input), so clear the preferred input;
    // any other explicit input means that's the desired sub-device.
    if (!deviceName.empty())
        mPreferredOutputDevice = deviceName;
    if (inputDeviceName == "__none__")
        mPreferredInputDevice.clear();
    else if (!inputDeviceName.empty())
        mPreferredInputDevice = inputDeviceName;

    // Per-driver memory: record the just-opened device under the
    // driver JUCE actually opened on. switchDriver reads this to
    // delegate driver-only picks to an explicit-name switchDevice,
    // closing the alphabetical-first-auto-pick hazard. Use JUCE's
    // type directly — currentDriver() hides the intent fallback.
    if (!deviceName.empty() && mDeviceManager) {
        if (auto* dev = mDeviceManager->getCurrentAudioDevice()) {
            std::string drv = dev->getTypeName().toStdString();
            if (!drv.empty())
                mPreferredDeviceByDriver[drv] = deviceName;
            mIntendedDriver.clear();
        }
    }
}

std::string SupersonicEngine::refuseUnknownDeviceName(
        const std::string& deviceName,
        const std::string& inputDeviceName) {
    if (!mDeviceManager) return {};
    if (deviceName.empty() && inputDeviceName.empty()) return {};
    std::vector<std::string> visibleNames;
    for (auto& d : listDevices(false)) visibleNames.push_back(d.name);
    return sonicpi::device::validateSwapDeviceNames(
        deviceName, inputDeviceName, visibleNames);
}

std::string SupersonicEngine::refuseWirelessMicAddition(
        const std::string& deviceName,
        const std::string& inputDeviceName) {
#ifdef __APPLE__
    // Only applies to "add mic while keeping current output" swaps.
    if (!deviceName.empty()) return {};
    if (inputDeviceName.empty() || inputDeviceName == "__none__") return {};
    if (!mDeviceManager) return {};
    auto* cur = mDeviceManager->getCurrentAudioDevice();
    if (!cur) return {};
    std::string curOut = mRealOutputDeviceName.empty()
        ? cur->getName().toStdString()
        : mRealOutputDeviceName;
    for (auto& dev : listDevices(false)) {
        if (sameDeviceName(dev.name, curOut) && !dev.isSuitableForAggregate()) {
            std::string err = "can't add input '" + inputDeviceName
                            + "' — current output '" + curOut
                            + "' is wireless and can't be aggregated with a mic";
            fprintf(stderr, "[switchDevice] %s\n", err.c_str());
            fflush(stderr);
            return err;
        }
    }
#else
    (void)deviceName; (void)inputDeviceName;
#endif
    return {};
}

std::unique_ptr<juce::AudioDeviceManager>
SupersonicEngine::makeDeviceManager() const {
    if (mCurrentConfig.deviceManagerFactory)
        return mCurrentConfig.deviceManagerFactory();
    return std::make_unique<juce::AudioDeviceManager>();
}

std::string SupersonicEngine::probeDriverTypeName(
        const sonicpi::device::SwapScope& scope) const {
    if (scope.crossDriver) return scope.targetDriver;
    if (!mDeviceManager) return {};
    if (auto* dev = mDeviceManager->getCurrentAudioDevice())
        return dev->getTypeName().toStdString();
    return mDeviceManager->getCurrentAudioDeviceType().toStdString();
}

int SupersonicEngine::probeDeviceChannelCount(const std::string& name,
                                              bool isInput,
                                              const std::string& typeName) {
    if (name.empty() || name == "__none__") return -1;
    if (!mDeviceManager) return -1;

    // The currently open device answers from its live handle: authoritative
    // and free. Its cache entry is never probed (listDevices must not
    // createDevice on a device that is already open), and creating a second
    // instance below can fail or disturb the live one on some drivers.
    if (auto* cur = mDeviceManager->getCurrentAudioDevice()) {
        if (sameDeviceName(cur->getName().toStdString(), name)
            && (typeName.empty()
                || cur->getTypeName().toStdString() == typeName)) {
            const int n = isInput ? cur->getInputChannelNames().size()
                                  : cur->getOutputChannelNames().size();
            if (n > 0) return n;
        }
    }

    // The enumerated device list already carries both channel counts and is
    // cached, so when the caller knows which driver it means, the answer is
    // already in hand. Falling through to createDevice() below instead costs
    // a full driver init to re-read a cached number, around 0.7 s on ASIO,
    // and worse for a driver whose hardware is registered but not plugged
    // in.
    //
    // Only consulted when typeName is known: the same device name appears
    // under several driver types with different channel counts (a MOTU is
    // 2-in under ASIO and 24-in under WASAPI), so an untyped lookup could
    // return a count belonging to a driver the swap does not target. And
    // only trusted when the count was genuinely probed: entries that skipped
    // probing carry placeholder counts, good enough for a device list but
    // not for the swap decisions this feeds.
    if (!typeName.empty()) {
        for (const auto& dev : listDevices(false)) {
            if (dev.typeName != typeName) continue;
            if (!sameDeviceName(dev.name, name)) continue;
            if (!(isInput ? dev.inChannelsProbed : dev.outChannelsProbed)) continue;
            const int n = isInput ? dev.maxInputChannels : dev.maxOutputChannels;
            if (n > 0) return n;
        }
    }

    auto& types = mDeviceManager->getAvailableDeviceTypes();
    for (auto* type : types) {
        for (auto& n : type->getDeviceNames(isInput)) {
            if (!sameDeviceName(n.toStdString(), name)) continue;
            auto outArg = isInput ? juce::String() : n;
            auto inArg  = isInput ? n : juce::String();
            std::unique_ptr<juce::AudioIODevice> probe(
                type->createDevice(outArg, inArg));
            if (probe)
                return isInput ? probe->getInputChannelNames().size()
                               : probe->getOutputChannelNames().size();
        }
    }
    return -1;
}

std::vector<double> SupersonicEngine::probeDeviceSampleRates(
        const std::string& name, bool isInput) {
    std::vector<double> result;
    // Serialise mDeviceManager access against swaps/recovery — the
    // scanForDevices() below mutates each type's cached device lists, which
    // listDevices() readers iterate. Same gate discipline as listDevices().
    std::lock_guard<std::recursive_mutex> gate(mSwapMutex);
    if (name.empty() || !mDeviceManager) return result;
    auto& types = mDeviceManager->getAvailableDeviceTypes();
    for (auto* type : types) {
        type->scanForDevices();
        for (auto& n : type->getDeviceNames(isInput)) {
            if (!sameDeviceName(n.toStdString(), name)) continue;
            auto outArg = isInput ? juce::String() : n;
            auto inArg  = isInput ? n : juce::String();
            std::unique_ptr<juce::AudioIODevice> probe(
                type->createDevice(outArg, inArg));
            if (!probe) return result;
            for (auto r : probe->getAvailableSampleRates())
                result.push_back(r);
            return result;
        }
    }
    return result;
}


void SupersonicEngine::clampAggregateBufferIfNeeded(int& bufferSize) {
#ifdef __APPLE__
    const bool active = AggregateDeviceHelper::exists()
                     && AggregateDeviceHelper::driftCompensationEnabled();
    const int clamped = sonicpi::device::clampBufferForDriftComp(bufferSize, active);
    if (clamped != bufferSize) {
        fprintf(stderr, "[device-setup] clamping aggregate buffer "
                "%d -> %d (drift-comp minimum)\n",
                bufferSize, clamped);
        fflush(stderr);
        bufferSize = clamped;
        mCurrentConfig.bufferSize = clamped;
    }
#endif
}

int SupersonicEngine::aggregateInputChannelOffsetFor(
        const std::string& outputDeviceName) const {
    if (outputDeviceName.empty()) return 0;
    for (auto& d : listDevices(false)) {
        if (d.name == outputDeviceName) return d.maxInputChannels;
    }
    return 0;
}

// Routine lifecycle logging (state transitions, block size, callback start) is
// handy when bringing a device up by hand but is pure noise in CI test runs —
// printed on every boot/shutdown of every test. Route it through here so a
// single SUPERSONIC_QUIET=1 (set in CI) silences it; warnings and errors keep
// using fprintf(stderr) directly and are never gated.
//
// Native-only host logging to the terminal/CI. ss_log() instead frames
// /supersonic/debug onto the OUT ring (the host's debug channel) and no-ops
// until memory is initialised, so it would drop these early-boot lines.
static void ssLifecycleLog(const char* fmt, ...) {
    static const bool quiet = [] {
        const char* v = std::getenv("SUPERSONIC_QUIET");
        return v != nullptr && v[0] != '\0' && v[0] != '0';
    }();
    if (quiet) return;
    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
    fflush(stderr);
}

#if defined(__linux__) && defined(SUPERSONIC_PIPEWIRE)
// Touch the type list before adding ours: JUCE only creates its built-in
// types (ALSA/JACK) when the list is empty, so appending first would leave
// PipeWire as the sole driver.
static void registerPipeWireDriver(juce::AudioDeviceManager& dm) {
    dm.getAvailableDeviceTypes();
    if (auto pwType = createPipeWireAudioIODeviceType())
        dm.addAudioDeviceType(std::move(pwType));
}

// Prefer the native PipeWire driver whenever the daemon exposes devices: it
// shows friendly device names, follows the system default sink/source, and
// needs neither pipewire-alsa nor pipewire-jack compat layers installed.
// ALSA and JACK remain selectable via /supersonic/drivers/switch. The scan
// also matters on its own: JUCE settles default-device init on the first
// type that reports devices, and a never-scanned type reports none.
static void preferPipeWireDriverIfAvailable(juce::AudioDeviceManager& dm) {
    for (auto* t : dm.getAvailableDeviceTypes()) {
        if (t->getTypeName() != "PipeWire")
            continue;
        t->scanForDevices();
        if (t->getDeviceNames(false).size() > 0)
            dm.setCurrentAudioDeviceType("PipeWire", true);
        break;
    }
}
#endif

void SupersonicEngine::setEngineState(EngineState state, const std::string& reason) {
    EngineState prev = mEngineState.exchange(state);
    if (prev == state) return;  // no transition

    const char* stateStr = engineStateToString(state);
    ssLifecycleLog("[supersonic] state: %s -> %s (%s)\n",
                   engineStateToString(prev), stateStr,
                   reason.empty() ? "-" : reason.c_str());

    mEgress.sendStateChange(stateStr, reason.c_str());

    // /supersonic/setup fires ONLY when the World was actually rebuilt
    // (cold swap). Spider receives this and runs cold_swap_reinit! which
    // resets all node IDs. Sending it on swap-failed-rollback or hot swaps
    // (where old nodes still exist) would cause duplicate node ID errors.
    if (state == EngineState::Running && mWorldRebuilt) {
        mWorldRebuilt = false;
        // Bump before emit so the wire value is the post-rebuild
        // generation (mSetupGeneration starts at 1; first cold swap = 2).
        uint32_t gen = mSetupGeneration.fetch_add(1) + 1;
        auto* dev = mDeviceManager ? mDeviceManager->getCurrentAudioDevice() : nullptr;
        int sr  = dev ? static_cast<int>(dev->getCurrentSampleRate()) : mCurrentConfig.sampleRate;
        int buf = dev ? dev->getCurrentBufferSizeSamples() : mCurrentConfig.bufferSize;
        mEgress.sendSetup(sr, buf, gen);
    }
}

SupersonicEngine::~SupersonicEngine() {
    shutdown();
}

// Defined in MdaUGens.cpp — sets the global sample table the MdaPiano UGen reads.
extern "C" void supersonic_set_piano_wavetable(const short* data, size_t count);

// Read the raw little-endian int16 piano sample table into mPianoWavetable and
// hand it to the UGen. Runs on the boot thread (never the audio thread). On any
// failure the table stays empty and :piano simply plays silence.
void SupersonicEngine::loadPianoWavetable(const std::string& path) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) {
        ssLifecycleLog("[piano] could not open wavetable: %s\n", path.c_str());
        return;
    }
    std::streamsize bytes = f.tellg();
    if (bytes <= 0 || (bytes % (std::streamsize)sizeof(short)) != 0) {
        ssLifecycleLog("[piano] bad wavetable size (%lld bytes): %s\n", (long long)bytes, path.c_str());
        return;
    }
    f.seekg(0);
    mPianoWavetable.resize((size_t)(bytes / (std::streamsize)sizeof(short)));
    if (!f.read(reinterpret_cast<char*>(mPianoWavetable.data()), bytes)) {
        ssLifecycleLog("[piano] failed reading wavetable: %s\n", path.c_str());
        mPianoWavetable.clear();
        return;
    }
    supersonic_set_piano_wavetable(mPianoWavetable.data(), mPianoWavetable.size());
    ssLifecycleLog("[piano] wavetable loaded: %zu samples\n", mPianoWavetable.size());
}


void SupersonicEngine::init(const Config& cfg) {
    if (mRunning.load()) return;
    setEngineState(EngineState::Booting, "init");

    if (!cfg.appName.empty())
        sPublishedAppName = cfg.appName;

    mHeadless = cfg.headless;
    mSuperClock.setFreewheelClock(cfg.freewheelClock);
    mCurrentConfig = cfg;
    // mBootInputChannels may be kAutoChannelCount (-1) here — resolved to a
    // concrete count when enableInputChannels() is eventually called.
    mBootInputChannels = cfg.numInputChannels;

    // Load the MdaPiano sample table off the audio thread (we're on the boot
    // thread here). Injected into the UGen; safe to call before World_New since
    // the setter only stores a global pointer.
    if (mPianoWavetable.empty() && !cfg.pianoWavetablePath.empty())
        loadPianoWavetable(cfg.pianoWavetablePath);

    // Seed the pre-wireless rate from the boot config so that a user who
    // boots directly into a wireless default (e.g. AirPlay because no
    // other output is present) still has a rate to restore when they
    // later switch to a hardware device. Without this seed, the first
    // non-wireless switch after a boot-on-wireless has mPreWirelessRate=0
    // and the AirPlay-negotiated 44.1 kHz sticks onto the new device.
    mPreWirelessRate = cfg.sampleRate;

    // Seed the preferred-output-device name from -H / sound_card_name so
    // that a boot with the device absent (USB interface unplugged, etc.)
    // still remembers the user's intent — the device-list-change listener
    // will auto-switch to it when it reappears. "__system__" sentinel means
    // "explicitly follow macOS default"; leave the preferred empty.
    if (!cfg.hardwareDevice.empty() && cfg.hardwareDevice != "__system__")
        mPreferredOutputDevice = cfg.hardwareDevice;
    // Same for the requested input (-H's input name): remembered as the
    // preferred input so boot pairing and hotplug re-attach both honour it.
    // Only when inputs are enabled — with -i 0 (or the macOS mic-permission
    // guard's zeroing), a seeded preference would let decideHotplugAction
    // re-open the very input stream the disable exists to avoid.
    if (cfg.numInputChannels != 0
        && !cfg.inputDevice.empty() && cfg.inputDevice != "__none__")
        mPreferredInputDevice = cfg.inputDevice;

    if (!cfg.headless) initAudioDevice(cfg);
    initEngine(cfg);
}

// Boot half 1: open the audio device. Everything here is device-side —
// driver selection, the -H named open, the default-device open with its
// wireless-avoiding fallback, input pairing / aggregate promotion, and
// rate/buffer negotiation. No World, no rings: the engine half
// (initEngine) can assume whatever device (or none) this settled on.
// The known boot/swap duplication (open ladders, full-duplex retry,
// aggregate creation vs switchDevice's) lives HERE and is the remaining
// unification target: boot should ultimately build a SwapPlan and run
// the shared executor once the executor can run without a World.
void SupersonicEngine::initAudioDevice(const Config& cfg) {
    // Map -1 (auto/max) to a large request count. JUCE/CoreAudio will clamp
    // the bitmask to the actual device channel count, so asking for more
    // than exists is safe; the callback's active-channels query later reads
    // the real count back.
    auto resolveReq = [](int n) -> int {
        return n < 0 ? kRequestMaxChannels : n;
    };
    int reqIn  = resolveReq(cfg.numInputChannels);
    int reqOut = resolveReq(cfg.numOutputChannels);

    // Every platform-specific piece of bring-up is skipped under the
    // deviceManagerFactory seam — an injected (fake-device) manager
    // must not touch real CoreAudio/PipeWire/DirectSound state.
    const bool platformSetup = !cfg.deviceManagerFactory;
#ifdef __APPLE__
    if (platformSetup) {
        // Tell CoreAudio to deliver HAL property notifications on its
        // own internal thread instead of the main CFRunLoop.  Required
        // because we don't run a Cocoa event loop.
        {
            CFRunLoopRef nullRunLoop = NULL;
            AudioObjectPropertyAddress prop = {
                kAudioHardwarePropertyRunLoop,
                kAudioObjectPropertyScopeGlobal,
                kAudioObjectPropertyElementMain
            };
            AudioObjectSetPropertyData(kAudioObjectSystemObject, &prop,
                                       0, NULL, sizeof(CFRunLoopRef),
                                       &nullRunLoop);
        }

        // Clean up any orphaned aggregate device from a previous crash
        // before initialising the audio device manager.
        AggregateDeviceHelper::cleanupOrphaned();
    }
#endif
    mDeviceManager = makeDeviceManager();

#ifdef __linux__
    if (platformSetup) {
        silenceJackLogsIfPossible();
#ifdef SUPERSONIC_PIPEWIRE
        registerPipeWireDriver(*mDeviceManager);
#endif
    }
#endif

    // -- Select audio driver --------------------------------------------------
    {
        auto& types = mDeviceManager->getAvailableDeviceTypes();
        fprintf(stderr, "  Available drivers:");
        for (auto* t : types)
            fprintf(stderr, " [%s]", t->getTypeName().toRawUTF8());
        fprintf(stderr, "\n");
        fflush(stderr);
    }

#if defined(__linux__) && defined(SUPERSONIC_PIPEWIRE)
    if (platformSetup) {
        preferPipeWireDriverIfAvailable(*mDeviceManager);
        if (mDeviceManager->getCurrentAudioDeviceType() == "PipeWire")
            mBootDriver = "PipeWire";
    }
#endif

#ifdef _WIN32
    // Default to DirectSound on Windows when no driver preference is
    // supplied. Historical default, not verified policy: the WASAPI
    // shared-mode crackle it was meant to avoid is unconfirmed, and
    // DirectSound has shown its own failure modes on real hardware
    // (clock rate skew, wedged device threads). A --audio-driver
    // request overrides this below.
    if (platformSetup) {
        auto& types = mDeviceManager->getAvailableDeviceTypes();
        for (auto* t : types) {
            if (t->getTypeName() == "DirectSound") {
                mDeviceManager->setCurrentAudioDeviceType("DirectSound", true);
                mBootDriver = "DirectSound";
                break;
            }
        }
    }
#endif

    // Honour the requested boot driver (--audio-driver): the GUI's saved
    // driver preference, forwarded by the daemon so the engine opens on
    // it directly instead of booting the platform default above and
    // being cold-swapped over after the boot handshake. Resolution is
    // policy (resolveBootDriver): exact type-name match, then unique
    // case-insensitive; ASIO is refused without a -H device (it has no
    // default device, and probing one can hang in IASIO::init). An
    // unresolvable name keeps the platform default with a warning. Runs
    // under the factory seam too — an injected manager owns its own
    // types and the request resolves (or warns) against those.
    bool bootDriverRequested = false;
    if (!cfg.audioDriver.empty()) {
        std::vector<std::string> typeNames;
        for (auto* t : mDeviceManager->getAvailableDeviceTypes())
            typeNames.push_back(t->getTypeName().toStdString());
        const bool hasDeviceRequest = !cfg.hardwareDevice.empty()
                                   && cfg.hardwareDevice != "__system__";
        auto choice = sonicpi::device::resolveBootDriver(
            cfg.audioDriver, typeNames, hasDeviceRequest);
        if (!choice.warning.empty()) {
            fprintf(stderr, "[device-setup] %s\n", choice.warning.c_str());
            fflush(stderr);
        }
        if (!choice.driver.empty()) {
            mDeviceManager->setCurrentAudioDeviceType(
                juce::String(choice.driver), true);
            mBootDriver = choice.driver;
            bootDriverRequested = true;
            ssLifecycleLog("[device-setup] boot driver: '%s'\n",
                           choice.driver.c_str());
        }
    }

    // -H "__system__" is the GUI sentinel for "follow macOS default".
    // Skip fuzzy-match and go straight to initialiseWithDefaultDevices.
    juce::String initError;
    bool openedByHardwareFlag = false;

    if (!cfg.hardwareDevice.empty() && cfg.hardwareDevice != "__system__") {
        struct DevEntry { std::string combined, typeName, devName; };
        std::vector<DevEntry> entries;

        auto& types = mDeviceManager->getAvailableDeviceTypes();
        for (auto* type : types) {
            type->scanForDevices();
            for (auto& name : type->getDeviceNames(false)) {
                DevEntry e;
                e.typeName = type->getTypeName().toStdString();
                e.devName  = name.toStdString();
                e.combined = e.typeName + " : " + e.devName;
                entries.push_back(e);
            }
        }

#ifdef __APPLE__
        // Filter wireless (AirPlay/Bluetooth) from fuzzy-match candidates.
        // These can't be opened via HAL — the route is only warmed up when
        // the device becomes the macOS system default via System Settings.
        {
            auto allDevs = listDevices();
            std::set<std::string> wirelessNames;
            for (auto& d : allDevs)
                if (d.isWirelessTransport()) wirelessNames.insert(d.name);

            entries.erase(std::remove_if(entries.begin(), entries.end(),
                [&wirelessNames](const DevEntry& e) {
                    for (auto& w : wirelessNames)
                        if (sameDeviceName(e.devName, w)) return true;
                    return false;
                }), entries.end());
        }
#endif

        // Resolve the requested device, scoped to the requested driver
        // when one was honoured above — an unscoped match resolves by
        // shortest combined name, which lands a bare device name on
        // whichever driver has the shortest NAME rather than the one
        // the user chose (see resolveBootHardwareMatch).
        std::vector<std::pair<std::string, std::string>> deviceTable;
        for (auto& e : entries)
            deviceTable.emplace_back(e.typeName, e.devName);
        std::string matched = sonicpi::device::resolveBootHardwareMatch(
            cfg.hardwareDevice,
            bootDriverRequested ? mBootDriver : std::string(),
            deviceTable);
        if (matched.empty()) {
            fprintf(stderr,
                    "[device-setup] WARNING: requested output device '%s' not found. "
                    "Falling back to system default. Available outputs:\n",
                    cfg.hardwareDevice.c_str());
            for (auto& e : entries)
                fprintf(stderr, "    %s\n", e.combined.c_str());
        } else {
            for (auto& e : entries) {
                if (e.combined != matched) continue;

                mDeviceManager->setCurrentAudioDeviceType(
                    juce::String(e.typeName), true);

                juce::AudioDeviceManager::AudioDeviceSetup setup;
                setup.outputDeviceName = juce::String(e.devName);
                setup.inputDeviceName  = juce::String();
                setup.useDefaultOutputChannels = true;
                setup.useDefaultInputChannels  = false;
                // Requested input is this same device (full-duplex, e.g.
                // a virtual loopback): open both directions in one go —
                // no aggregate needed, and no transient default-input
                // state for the GUI to "correct" with a cold swap.
                // Strict exact-or-"(N)" resolution against the full
                // enumeration: with "USB Audio Device" and "USB Audio
                // Device (2)" both attached, the exact entry wins, so
                // opening box 2 never swallows box 1's input role.
                if (cfg.numInputChannels != 0
                    && !mPreferredInputDevice.empty()) {
                    std::vector<std::string> devNames;
                    for (auto& cand : entries)
                        devNames.push_back(cand.devName);
                    if (sonicpi::device::resolveJuceDeviceName(
                            mPreferredInputDevice, devNames) == e.devName) {
                        setup.inputDeviceName = juce::String(e.devName);
                        setup.useDefaultInputChannels = true;
                    }
                }
                if (cfg.sampleRate > 0) setup.sampleRate = cfg.sampleRate;
                if (cfg.bufferSize > 0) setup.bufferSize = cfg.bufferSize;

                // Request input channels only when the setup names an
                // input — a -H boot never opens an implicit one. On macOS
                // a blank name with reqIn > 0 made JUCE pair the default
                // mic via its Combiner (#3554); elsewhere it named a
                // default input but activated zero channels anyway
                // (useDefaultInputChannels=false + empty mask). Explicit
                // input intent is the uniform contract; the input-pairing
                // blocks below attach preferred inputs on all platforms.
                const int hwReqIn =
                    setup.inputDeviceName.isNotEmpty() ? reqIn : 0;
                initError = mDeviceManager->initialise(
                    hwReqIn, reqOut,
                    nullptr, false, juce::String(), &setup);

                // A failed full-duplex attempt may be the input half
                // alone (mic-privacy denial, exclusive-mode contention,
                // input-side rate limits). Retry output-only — same
                // rescue as switchDevice's input-fallback — so a bad
                // input never costs the user their chosen output;
                // aggregate promotion below pairs an input normally.
                if (initError.isNotEmpty()
                    && setup.inputDeviceName.isNotEmpty()) {
                    fprintf(stderr, "[device-setup] -H full-duplex open of "
                            "'%s' failed (%s) — retrying output-only\n",
                            e.devName.c_str(), initError.toRawUTF8());
                    fflush(stderr);
                    setup.inputDeviceName = juce::String();
                    setup.useDefaultInputChannels = false;
                    initError = mDeviceManager->initialise(
                        0, reqOut,
                        nullptr, false, juce::String(), &setup);
                }

                if (initError.isNotEmpty()) {
                    fprintf(stderr, "[device-setup] -H '%s' matched '%s' but failed: %s\n",
                            cfg.hardwareDevice.c_str(), e.combined.c_str(),
                            initError.toRawUTF8());
                } else {
                    fprintf(stderr, "  -H '%s' -> %s\n",
                            cfg.hardwareDevice.c_str(), e.combined.c_str());
                    mDeviceMode = e.devName;
                    mBootDriver = e.typeName;
                    openedByHardwareFlag = true;
                }
                break;
            }
        }
    }

    if (!openedByHardwareFlag) {
#ifdef __APPLE__
        // On macOS, boot output-only then create an Aggregate Device.
        // JUCE's AudioIODeviceCombiner (used when input and output are
        // different hardware devices) is unreliable at small buffer sizes.
        //
        // Pre-check: if the macOS system default is wireless (AirPlay
        // / Bluetooth), do NOT open it at boot. Opening wireless then
        // transitioning to a non-wireless device for the aggregate
        // triggers a ~15 s CoreAudio IOProc halt — Sonic Pi's boot
        // handshake times out in that window and scopes never start.
        // Pick a non-wireless fallback up front.
        std::string bootFallback;
        {
            AudioDeviceID defaultID = kAudioObjectUnknown;
            AudioObjectPropertyAddress addr = {
                kAudioHardwarePropertyDefaultOutputDevice,
                kAudioObjectPropertyScopeGlobal,
                kAudioObjectPropertyElementMain
            };
            UInt32 sz = sizeof(defaultID);
            if (AudioObjectGetPropertyData(kAudioObjectSystemObject,
                    &addr, 0, nullptr, &sz, &defaultID) == noErr
                && defaultID != kAudioObjectUnknown) {
                // Default name
                CFStringRef cfName = nullptr;
                UInt32 nsz = sizeof(cfName);
                AudioObjectPropertyAddress nameAddr = {
                    kAudioDevicePropertyDeviceNameCFString,
                    kAudioObjectPropertyScopeGlobal,
                    kAudioObjectPropertyElementMain
                };
                std::string defaultName;
                if (AudioObjectGetPropertyData(defaultID, &nameAddr,
                        0, nullptr, &nsz, &cfName) == noErr && cfName) {
                    char buf[256];
                    CFStringGetCString(cfName, buf, sizeof(buf),
                                       kCFStringEncodingUTF8);
                    CFRelease(cfName);
                    defaultName = buf;
                }
                // Transport type
                AudioObjectPropertyAddress tAddr = {
                    kAudioDevicePropertyTransportType,
                    kAudioObjectPropertyScopeGlobal,
                    kAudioObjectPropertyElementMain
                };
                UInt32 tType = 0, tSize = sizeof(tType);
                bool defaultIsWireless = false;
                if (AudioObjectGetPropertyData(defaultID, &tAddr, 0,
                        nullptr, &tSize, &tType) == noErr) {
                    defaultIsWireless = CoreAudioTransport::isWireless(tType);
                }
                if (defaultIsWireless && !defaultName.empty()) {
                    std::vector<std::string> names;
                    std::vector<bool> wirelessFlags;
                    for (auto& d : listDevices()) {
                        names.push_back(d.name);
                        wirelessFlags.push_back(d.isWirelessTransport());
                    }
                    bootFallback = sonicpi::device::selectBootOutputDevice(
                        defaultName, defaultIsWireless, names, wirelessFlags);
                    if (!bootFallback.empty()) {
                        fprintf(stderr, "[device-setup] boot: default '%s' "
                                "is wireless; using non-wireless fallback '%s'\n",
                                defaultName.c_str(), bootFallback.c_str());
                        fflush(stderr);
                    } else {
                        fprintf(stderr, "[device-setup] boot: default '%s' "
                                "is wireless and no non-wireless fallback "
                                "available — opening wireless default may "
                                "silence audio for ~15 s during boot handshake\n",
                                defaultName.c_str());
                        fflush(stderr);
                    }
                }
            }
        }

        if (!bootFallback.empty()) {
            juce::AudioDeviceManager::AudioDeviceSetup setup;
            setup.outputDeviceName = juce::String(bootFallback);
            setup.useDefaultOutputChannels = true;
            initError = mDeviceManager->initialise(
                0, reqOut, nullptr, false, juce::String(), &setup);
            if (initError.isEmpty()) mDeviceMode = bootFallback;
        } else {
            initError = mDeviceManager->initialiseWithDefaultDevices(
                0, reqOut);
        }
        if (initError.isNotEmpty()) {
            fprintf(stderr, "[device-setup] init with 0 in / %d out failed: %s\n",
                    reqOut, initError.toRawUTF8());
            initError = mDeviceManager->initialiseWithDefaultDevices(0, 0);
        }
        // Aggregate-device creation runs below — for both the -H path
        // and this default-device path.
#else
        initError = mDeviceManager->initialiseWithDefaultDevices(
            reqIn, reqOut);
        if (initError.isNotEmpty()) {
            fprintf(stderr, "[device-setup] init with %d in / %d out failed: %s\n",
                    reqIn, reqOut,
                    initError.toRawUTF8());
            initError = mDeviceManager->initialiseWithDefaultDevices(0, 2);
        }
        if (initError.isNotEmpty()) {
            fprintf(stderr, "[device-setup] init with 0 in / 2 out failed: %s\n",
                    initError.toRawUTF8());
            initError = mDeviceManager->initialiseWithDefaultDevices(0, 0);
        }
#endif
        if (initError.isNotEmpty()) {
            fprintf(stderr, "[device-setup] all init attempts failed: %s\n",
                    initError.toRawUTF8());
        }
    }

#ifdef __APPLE__
    // Aggregate-promotion runs after both open paths (-H match and
    // default-device fallback). Without it, the -H path leaves the
    // engine on an output-only device while the config requests
    // inputs — some Macs never deliver callbacks on that mismatch,
    // stalling boot until an external swap arrives.
    if (initError.isEmpty() && cfg.numInputChannels != 0) {
        auto* dev = mDeviceManager->getCurrentAudioDevice();
        // A full-duplex -H open already carries its input: nothing to
        // promote — and the same-name fallback below would clobber the
        // opened device with a default-device re-open.
        if (dev && dev->getActiveInputChannels().countNumberOfSetBits() > 0)
            dev = nullptr;
        if (dev) {
            std::string outName = dev->getName().toStdString();
            std::string inName;
            AudioObjectPropertyAddress pa = {
                kAudioHardwarePropertyDefaultInputDevice,
                kAudioObjectPropertyScopeGlobal,
                kAudioObjectPropertyElementMain
            };
            AudioDeviceID inputDevId = 0;
            UInt32 sz = sizeof(inputDevId);
            if (AudioObjectGetPropertyData(kAudioObjectSystemObject,
                    &pa, 0, nullptr, &sz, &inputDevId) == noErr
                    && inputDevId != 0) {
                CFStringRef cfName = nullptr;
                UInt32 nsz = sizeof(cfName);
                AudioObjectPropertyAddress nameAddr = {
                    kAudioDevicePropertyDeviceNameCFString,
                    kAudioObjectPropertyScopeGlobal,
                    kAudioObjectPropertyElementMain
                };
                if (AudioObjectGetPropertyData(inputDevId, &nameAddr,
                        0, nullptr, &nsz, &cfName) == noErr && cfName) {
                    char buf[256];
                    CFStringGetCString(cfName, buf, sizeof(buf),
                                       kCFStringEncodingUTF8);
                    CFRelease(cfName);
                    inName = buf;
                }
            }
            // One cached-list snapshot serves the pairing choice and the
            // suitability check below — a rescan here can disrupt the
            // just-opened device (see listDevices).
            const auto bootDevices = listDevices(false);
            // Pair with the requested input (-H's input name) when it's
            // attached; the system default is the fallback, not the policy.
            if (!mPreferredInputDevice.empty()) {
                std::vector<std::string> inputNames;
                std::vector<bool> inputSuitable;
                for (auto& d : bootDevices) {
                    if (d.maxInputChannels <= 0) continue;
                    inputNames.push_back(d.name);
                    // Same vetting as switchDevice: never pair a wireless
                    // input into an aggregate (HFP 16 kHz mono; IOProc
                    // freeze). The opened output itself is exempt —
                    // same-device full duplex needs no aggregate.
                    inputSuitable.push_back(d.isSuitableForAggregate()
                                            || d.name == outName);
                }
                std::string chosen = sonicpi::device::chooseBootInputDevice(
                    mPreferredInputDevice, inName, inputNames, inputSuitable);
                if (chosen != inName) {
                    fprintf(stderr, "[device-setup] boot: pairing requested "
                            "input '%s' (system default '%s')\n",
                            chosen.c_str(), inName.c_str());
                    fflush(stderr);
                }
                inName = chosen;
            }
            // Skip aggregate for wireless (Bluetooth/AirPlay) outputs —
            // same rule (isSuitableForAggregate) as switchDevice; virtual
            // outputs aggregate fine with a hardware clock master. Boot
            // with output-only instead so we don't crash JUCE's Combiner
            // fallback when sample-rate negotiation fails.
            bool outputSuitable = true;
            if (!inName.empty() && inName != outName) {
                for (auto& d : bootDevices) {
                    if (d.name == outName && !d.isSuitableForAggregate()) {
                        outputSuitable = false;
                        fprintf(stderr, "[device-setup] boot: skipping aggregate — "
                                "'%s' is not aggregable (wireless or "
                                "aggregate-class); input disabled\n",
                                outName.c_str());
                        fflush(stderr);
                        break;
                    }
                }
            }
            if (!inName.empty() && inName != outName && outputSuitable) {
                double aggRate = 0;
                auto aggName = AggregateDeviceHelper::createOrUpdate(
                    outName, inName,
                    static_cast<double>(mCurrentConfig.sampleRate),
                    &aggRate);
                if (!aggName.empty()) {
                    // Wait until JUCE can actually see the new aggregate
                    // before opening it — a fixed sleep races CoreAudio's
                    // device-list refresh, and opening too early errors
                    // "No such device" → fallback that drops the mic.
                    waitForDeviceVisible(aggName, 2000);
                    mRealOutputDeviceName = outName;
                    mRealInputDeviceName  = inName;
                    mLastInputDeviceName  = inName;
                    juce::AudioDeviceManager::AudioDeviceSetup setup;
                    mDeviceManager->getAudioDeviceSetup(setup);
                    // Open at the rate the aggregate actually settled on
                    // (the helper adopts the sub-devices' rate if they
                    // refused the desired one) — don't force the rejected
                    // rate and re-introduce aggregate SRC.
                    if (aggRate > 0) setup.sampleRate = aggRate;
                    setup.outputDeviceName = juce::String(aggName);
                    setup.inputDeviceName  = juce::String(aggName);
                    setup.useDefaultInputChannels = false;
                    clampAggregateBufferIfNeeded(setup.bufferSize);
                    const int inOffset = aggregateInputChannelOffsetFor(outName);
                    juce::BigInteger inputBits;
                    inputBits.setRange(inOffset, reqIn, true);
                    setup.inputChannels = inputBits;
                    if (inOffset > 0) {
                        fprintf(stderr, "[device-setup] aggregate input bits offset by %d "
                                "(output sub-device '%s' contributes %d input channels) — "
                                "active input range = [%d..%d]\n",
                                inOffset, outName.c_str(), inOffset,
                                inOffset, inOffset + reqIn - 1);
                        fflush(stderr);
                    }
                    auto aggErr = mDeviceManager->setAudioDeviceSetup(setup, true);
                    if (aggErr.isNotEmpty()) {
                        fprintf(stderr, "[device-setup] aggregate setup failed: %s — "
                                "falling back to Combiner\n", aggErr.toRawUTF8());
                        AggregateDeviceHelper::destroy();
                        mRealOutputDeviceName.clear();
                        mRealInputDeviceName.clear();
                        // Fall back to Combiner
                        mDeviceManager->initialiseWithDefaultDevices(
                            reqIn, reqOut);
                    } else {
                        fprintf(stderr, "[device-setup] booted with aggregate: "
                                "out='%s' in='%s'\n", outName.c_str(), inName.c_str());
                        // Suppress CFRunLoop until Spider has finished
                        // cold_swap_reinit — queued audioDeviceListChanged
                        // messages would trigger a second cold swap and
                        // crash ScopeOut2 during the rebuild.
                        mSuppressRunLoop.store(true);
                    }
                }
            } else if (inName == outName && !openedByHardwareFlag) {
                // Default-device boot where the preferred input IS the
                // opened output: reopen full duplex on that device by
                // name. Reopening system defaults here would discard a
                // wireless-avoiding bootFallback and reopen the
                // wireless default (~15 s IOProc halt). Never on the
                // -H path — its full-duplex open already ran (and
                // possibly fell back to output-only); reopening here
                // would discard the user's chosen output.
                juce::AudioDeviceManager::AudioDeviceSetup dupSetup;
                dupSetup.outputDeviceName = juce::String(outName);
                dupSetup.inputDeviceName  = juce::String(outName);
                dupSetup.useDefaultOutputChannels = true;
                dupSetup.useDefaultInputChannels  = true;
                initError = mDeviceManager->initialise(
                    reqIn, reqOut, nullptr, false, juce::String(),
                    &dupSetup);
                if (initError.isNotEmpty()) {
                    fprintf(stderr, "[device-setup] boot: full-duplex "
                            "reopen of '%s' failed: %s — falling back "
                            "to defaults\n",
                            outName.c_str(), initError.toRawUTF8());
                    fflush(stderr);
                    initError = mDeviceManager->initialiseWithDefaultDevices(
                        reqIn, reqOut);
                }
            }
        }
    }
#else
    // Pair the requested input (-H's input name) on non-mac drivers,
    // which open input and output as separate devices — no aggregate
    // involved; switchDevice already supports this cross-platform.
    // Without it the GUI reconciler sees intent != actual on every
    // boot and "corrects" with the redundant cold swap this feature
    // exists to remove.
    if (initError.isEmpty() && cfg.numInputChannels != 0
        && !mPreferredInputDevice.empty()
        && mDeviceManager->getCurrentAudioDevice()) {
        juce::AudioDeviceManager::AudioDeviceSetup setup;
        mDeviceManager->getAudioDeviceSetup(setup);
        const std::string currentIn = setup.inputDeviceName.toStdString();
        std::vector<std::string> inputNames;
        for (auto& d : listDevices(false))
            if (d.maxInputChannels > 0) inputNames.push_back(d.name);
        const std::string chosen = sonicpi::device::chooseBootInputDevice(
            mPreferredInputDevice, currentIn, inputNames);
        if (!chosen.empty() && chosen != currentIn) {
            fprintf(stderr, "[device-setup] boot: pairing requested input "
                    "'%s' (default was '%s')\n",
                    chosen.c_str(), currentIn.c_str());
            fflush(stderr);
            setup.inputDeviceName = juce::String(chosen);
            setup.useDefaultInputChannels = false;
            // Clamp the bitmask to the device's real capacity — WASAPI
            // rejects a setup asking for more inputs than exist, and
            // the auto-max sentinel requests kRequestMaxChannels.
            int wantIn = reqIn;
            const int probedIn = probeDeviceChannelCount(
                chosen, true, probeDriverTypeName());
            if (probedIn > 0 && probedIn < wantIn) wantIn = probedIn;
            juce::BigInteger inputBits;
            inputBits.setRange(0, wantIn, true);
            setup.inputChannels = inputBits;
            const juce::String pairErr =
                mDeviceManager->setAudioDeviceSetup(setup, true);
            if (pairErr.isNotEmpty()) {
                // Keep the working default input rather than unwinding
                // an otherwise-good boot.
                fprintf(stderr, "[device-setup] boot input pairing failed: "
                        "%s — keeping '%s'\n",
                        pairErr.toRawUTF8(), currentIn.c_str());
                fflush(stderr);
            } else {
                mLastInputDeviceName = chosen;
            }
        }
    }
#endif

    // Negotiate sample rate and buffer size.
    // scsynth processes in fixed 128-sample blocks, so a hardware buffer
    // that is a multiple of 128 avoids prefetch overhead and eliminates
    // NTP timing discontinuities at callback boundaries.
    if (auto* dev = mDeviceManager->getCurrentAudioDevice()) {
        juce::AudioDeviceManager::AudioDeviceSetup setup;
        mDeviceManager->getAudioDeviceSetup(setup);
        bool changed = false;

        // Clamp sample rate to what the device actually supports.
        // EXCEPTION: when running our managed CoreAudio aggregate, it is
        // already at the sub-devices' native rate (the helper adopts that
        // when they refuse the desired rate). Forcing cfg.sampleRate here
        // would make CoreAudio resample inside the aggregate IOProc
        // (audible distortion) and needlessly change the system rate, so
        // honour the aggregate's rate instead. (Aggregates also falsely
        // report cfg.sampleRate as "supported" via that very SRC, so the
        // getAvailableSampleRates check below can't catch this.)
        bool onManagedAggregate = false;
#ifdef __APPLE__
        onManagedAggregate = AggregateDeviceHelper::exists();
#endif
        if (!onManagedAggregate &&
            static_cast<int>(setup.sampleRate) != cfg.sampleRate) {
            auto rates = dev->getAvailableSampleRates();
            bool supported = false;
            for (auto r : rates) {
                if (static_cast<int>(r) == cfg.sampleRate) {
                    supported = true;
                    break;
                }
            }
            if (supported) {
                setup.sampleRate = cfg.sampleRate;
                changed = true;
            } else {
                fprintf(stderr, "[device-setup] requested sr %d not supported, "
                        "keeping %.0f\n", cfg.sampleRate, setup.sampleRate);
            }
        }

        if (cfg.bufferSize > 0) {
            // Honour the user's -z / -Z / TOML buffer size, clamped up
            // only when we're running a drift-comp aggregate (a stale
            // block_size=64 in the TOML would otherwise boot into a
            // drift storm).
            int wantedBuf = cfg.bufferSize;
            clampAggregateBufferIfNeeded(wantedBuf);
            setup.bufferSize = wantedBuf;
            changed = true;
        } else if (dev->getTypeName() != "DirectSound") {
            // Auto: pick the smallest available buffer that is at least
            // 128 samples. scsynth's block size matches the HW buffer on
            // native, so any size works. Buffers below 128 mostly add
            // callback overhead without a latency win.
            constexpr int kMinBuf = 128;
            auto sizes = dev->getAvailableBufferSizes();
            int best = 0;
            for (auto s : sizes) {
                if (s >= kMinBuf && s <= sonicpi::kMaxBlockSize) {
                    best = s;
                    break;  // sizes are sorted ascending
                }
            }
            if (best > 0 && best != dev->getCurrentBufferSizeSamples()) {
                setup.bufferSize = best;
                changed = true;
            }
        }

        if (changed) {
            juce::String setupErr = mDeviceManager->setAudioDeviceSetup(setup, true);
            if (setupErr.isNotEmpty()) {
                fprintf(stderr, "[device-setup] setAudioDeviceSetup error: %s\n",
                        setupErr.toRawUTF8());
                // Recover: reinitialise with defaults rather than leaving device broken
                fprintf(stderr, "[device-setup] recovering with device defaults\n");
                mDeviceManager->initialiseWithDefaultDevices(
                    reqIn, reqOut);
            }
        }
    }

    // Read what the device actually settled on and override config to match
    // — including bufferSize, which the cold-swap readback already syncs;
    // leaving it unset here made every pre-first-swap consumer (headless
    // driver configure, SwapResult.bufferSize, state events) read the
    // requested value instead of the real one.
    if (auto* dev = mDeviceManager->getCurrentAudioDevice()) {
        double sr   = dev->getCurrentSampleRate();
        mCurrentConfig.sampleRate        = static_cast<int>(sr);
        mCurrentConfig.bufferSize        = dev->getCurrentBufferSizeSamples();
        mCurrentConfig.numOutputChannels = dev->getOutputChannelNames().size();
        mCurrentConfig.numInputChannels  = dev->getInputChannelNames().size();
    } else {
        fprintf(stderr, "[supersonic] warning: no audio device available\n");
    }
    fflush(stderr);

    // Arm the post-boot quiet window ONCE, here, against the change
    // notifications boot's own opens/aggregate work will deliver after
    // mRunning goes true. Eleven scattered stamps used to do this — all
    // but the final one were dead (every consumer bails until mRunning),
    // and the final one is exactly this line.
    mLastSelfTriggeredChange = std::chrono::steady_clock::now();
}

// Boot half 2: bring up the engine around whatever initAudioDevice
// settled on (or headless). Shared memory, the scsynth World, SuperClock
// + SHM binding, reader drains (registered here, where the ring pointers
// exist, before the reader threads start), transports, subsystem routes,
// workers, the audio source, and finally the watchdog.
void SupersonicEngine::initEngine(const Config& cfg) {
    // Resolve any remaining auto-max sentinels to concrete counts before the
    // World is initialised. This covers headless mode and any path where the
    // device failed to open (readback block above didn't run).
    if (mCurrentConfig.numOutputChannels < 0) mCurrentConfig.numOutputChannels = 2;
    if (mCurrentConfig.numInputChannels  < 0) mCurrentConfig.numInputChannels  = 0;

    // -- Create shared memory (owned by engine, survives cold swaps) --------
    if (cfg.udpPort > 0) {
        server_shared_memory_creator::cleanup(cfg.udpPort);
        try {
            mShmemCreator = std::make_unique<server_shared_memory_creator>(
                cfg.udpPort, cfg.numControlBusChannels);
            // Tell init_memory()/World_New to reuse this instead of creating its own
            g_external_shared_memory = mShmemCreator.get();
            // Point the engine's whole shared_memory.h arena at the public
            // segment: rings, control, metrics, node-tree, audio taps and
            // scope all live there, observable cross-process for free.
            g_external_segment = mShmemCreator->get_base();
        } catch (const std::exception& e) {
            fprintf(stderr, "[supersonic] shared memory creation failed: %s\n", e.what());
            fflush(stderr);
            mShmemCreator.reset();
            g_external_segment = nullptr;
        }
    } else {
        g_external_segment = nullptr;
    }

    // -- Initialise scsynth World ------------------------------------------
    // scsynth's control block size. cfg.blockSize (scsynth's -z) always wins;
    // otherwise match the opened device's callback buffer when it's SMALLER
    // than the default block (chooseBlockSize) — one block per callback, no
    // prefetch / accumulator decoupling, for users who chose a low-latency
    // driver buffer. Never raise the block above kDefaultBlockSize: larger
    // buffers (Sonic Pi ships -Z 1024) keep the default block and the
    // decoupling machinery spans the difference — matching upward would
    // coarsen the control rate. initialiseWorld clamps to
    // [32, kMaxBlockSize]. WASM has no such knob — its block must equal the
    // 128-sample AudioWorklet render quantum.
    int chosenBufLen = cfg.blockSize;
    if (chosenBufLen <= 0) {
        int hwBuf = 0;
        if (mDeviceManager) {
            if (auto* dev = mDeviceManager->getCurrentAudioDevice())
                hwBuf = dev->getCurrentBufferSizeSamples();
        } else {
            hwBuf = mCurrentConfig.bufferSize;  // headless: the manual pump size
        }
        chosenBufLen = sonicpi::device::chooseBlockSize(
            hwBuf, sonicpi::kDefaultBlockSize, 32,
            sonicpi::kDefaultBlockSize);
    }
    ssLifecycleLog("[supersonic] scsynth block size = %d samples\n", chosenBufLen);

    // The arena is the public segment when one exists (so the whole
    // shared_memory.h blob — WorldOptions, rings, metrics, scope … — lives
    // cross-process), else the process-local ring_buffer_storage. init_memory()
    // resolves the same base from g_external_segment, so both agree.
    uint8_t* arena = g_external_segment ? g_external_segment : ring_buffer_storage;

    // Use actual device sample rate and channel counts (may differ from requested)
    mAudioCallback.initialiseWorld(
        arena,
        mCurrentConfig.sampleRate,
        mCurrentConfig.numOutputChannels,
        mCurrentConfig.numInputChannels,
        cfg.numBuffers,
        cfg.maxNodes,
        cfg.maxGraphDefs,
        cfg.maxWireBufs,
        cfg.numAudioBusChannels,
        cfg.numControlBusChannels,
        cfg.realTimeMemorySize,
        cfg.numRGens,
        cfg.udpPort,  // sharedMemoryID — names the shm segment "SuperSonic_<port>"
        chosenBufLen
    );

    // Move the clock state into the shared arena's SUPERCLOCK_STATE region so
    // the native SHM has the same shape as web. Done before publish() below,
    // so a cross-process reader sees it populated.
    mSuperClock.bindStateToShm(
        reinterpret_cast<SuperClockState*>(arena + SUPERCLOCK_STATE_START));
    mSuperClock.bindSampleClockToShm(arena + SAMPLE_CLOCK_START);

    // Seed the session tempo before any consumer can read the clock (the audio
    // source and /clock RPCs come up later) and before the tempo-changed callback
    // is installed, so the engine opens at the embedder's tempo with no notify and
    // no transient. Seeding here — rather than a post-boot /clock/tempo/set — is
    // what keeps bpm and beat_origin consistent from the first read: a later set
    // mirrors bpm immediately but re-anchors beat_origin on Link's thread async,
    // leaving a window where the two disagree. Skip when it equals the constructed
    // default so that path stays byte-for-byte unchanged.
    if (cfg.defaultBpm != supersonic::kDefaultBpm)
        mSuperClock.setBpm(cfg.defaultBpm);

    // The arena is populated by init_memory() (run synchronously inside
    // initialiseWorld). Publish the segment — store MAGIC last — so a
    // cross-process reader that observes MAGIC sees fully-initialised regions
    // (e.g. node-tree 0xFF empty markers), never the half-zeroed boot state.
    if (mShmemCreator)
        mShmemCreator->publish();

    // Publish the peer command plane only when enabled: the gateway task below
    // drains its command ring and ShmTransport sends through this slot.
    // Disabled (or no segment) ⇒ the slot stays null and both sides are inert.
    if (cfg.shmCommands) {
        if (mShmemCreator) {
            mPeerPlane.store(mShmemCreator->get_peer_plane(), std::memory_order_release);
        } else {
            fprintf(stderr, "[supersonic] WARNING: shmCommands requested but there is "
                            "no SHM segment (udpPort == 0) — command plane disabled\n");
        }
    }

    // Derive worker pointers from the arena. With the unified layout the rings,
    // control words and metrics all live in the arena (the public segment when
    // present), so external observers read the same structs with no redirect.
    uint8_t* base = arena;
    ControlPointers*    ctrl = reinterpret_cast<ControlPointers*>(base + CONTROL_START);
    mMetrics                 = reinterpret_cast<PerformanceMetrics*>(base + METRICS_START);

    // -- NRT gateway: drain #1 = the RT egress lane (OUT ring), via the lanes
    //    ABI — the gateway is its single consumer; the drain state, route
    //    peeling and metrics live in lanes.cpp. Woken every audio block via
    //    processCount. (Drain #2 = the control ring, added with the NRT plane
    //    below.)
    mNrtGateway.setWake(&mAudioCallback.processCount);
    mNrtGateway.addTask([this]() {
        ss_egress_rt_drain(
            [](void* ctx, uint32_t token, uint32_t route,
               const uint8_t* osc, uint32_t len, uint32_t) {
                static_cast<SupersonicEngine*>(ctx)->mEgress.dispatchEgress(
                    token, route, osc, len);
            },
            this, 0 /* drain everything available */);
    });

    // -- NRT gateway: peer command plane (SHM segment; shm_peer_plane.h) -----
    // One trusted external peer writes OSC frames into the plane's SPSC
    // command ring; this task feeds them into the ordinary ingest path (IN
    // ring → audio-thread classify → control forward), stamped with the
    // reserved SHM-peer origin token — identity is assigned here, never
    // trusted from shared memory. Registered before the control-ring drain so
    // a forwarded control command is handled the same wake. Bounded per wake
    // so a flooding peer cannot starve the other drains (the remainder drains
    // next block); a full IN ring Retains the frame — lossless backpressure
    // through to the peer's own ring-full signal. Inert while mPeerPlane is
    // null (shmCommands off).
    mNrtGateway.addTask([this]() {
        auto* plane = mPeerPlane.load(std::memory_order_acquire);
        if (!plane) return;
        constexpr uint32_t kPeerDrainMaxFrames = 256;
        ss_drain_ring(
            shm_peer_cmd_ring(plane), SHM_PEER_CMD_RING_SIZE,
            &plane->cmd_head, &plane->cmd_tail, mPeerDrainState,
            SsDrainMetrics{ nullptr, nullptr,
                            mMetrics ? &mMetrics->osc_in_corrupted : nullptr,
                            nullptr },
            kPeerDrainMaxFrames,
            [this](uint32_t /*frameSrc*/, const uint8_t* d, uint32_t n, uint32_t) {
                if (!ss_ingress_write(d, n, SHM_PEER_ORIGIN_TOKEN))
                    return SsDrainVerdict::Retain;   // IN ring full — retry next wake
                if (mMetrics) {
                    mMetrics->osc_out_messages_sent.fetch_add(1, std::memory_order_relaxed);
                    mMetrics->osc_out_bytes_sent.fetch_add(n, std::memory_order_relaxed);
                }
                return SsDrainVerdict::Consume;
            });
    });

    // -- Audio-plane ingress (engine-owned) ---------------------------------
    // The engine owns the IN ring; the default OscIngress route writes it. The
    // transport is a dumb pipe.
    mInBufferStart = base + IN_BUFFER_START;
    mInBufferSize  = IN_BUFFER_SIZE;
    mInHead        = &ctrl->in_head;
    mInTail        = &ctrl->in_tail;
    mInSequence    = &ctrl->in_sequence;
    mInWriteLock   = &ctrl->in_write_lock;

    // The engine owns no socket. By default egress is in-process (CallbackTransport
    // → onReply); the embedder injects a real transport via setTransport before
    // init() — the standalone server an UdpOscTransport.
    if (!mTransport)
        mTransport = &mDefaultTransport;

    // Egress is deferred: producers frame OSC into the NRT-out ring (via the
    // lanes producer, ss_egress_nrt_write); the gateway drains it and is the
    // sole transport caller. /supersonic/debug log lines surface via onDebug.
    mEgress.init(mTransport, &onDebug);

    // Pre-dispatch hook for both egress rings (/supersonic/buffer/freed deferred-free).
    mEgress.setInterceptor([this](const uint8_t* d, uint32_t n) { return interceptBufferFreed(d, n); });

    // Three address dispatchers, all OscIngress: the AUDIO-thread ingress (classifies
    // inbound OSC — control forwards to the NRT ring, synth is the default route),
    // the NRT control dispatcher (forwarded command → subsystem handler, default
    // = handleLinkCommand for /clock/), and the IO outbound dispatcher (below). Each
    // backend registers its routes on the planes it uses — one stanza per backend.
    mIngress.setDefault(&ss_synth_default_route, nullptr);
    mControlIngress.setDefault(&SupersonicEngine::routeTo<EngineControl, &EngineControl::handleLinkCommand>, &mEngineControl);
    mEngineControl.init(this, &mEgress, &mSuperClock);
    mIngress.registerRoute("/supersonic/", &SupersonicEngine::nrtForwardSink, this);
    mIngress.registerRoute("/clock/", &SupersonicEngine::nrtForwardSink, this);
    mControlIngress.registerRoute("/supersonic/", &SupersonicEngine::routeTo<EngineControl, &EngineControl::handleSupersonicCommand>, &mEngineControl);
#ifdef SUPERSONIC_MIDI
    // The clock-out coordinator is a process-wide singleton; a fresh engine owns
    // no clock-out ports, so clear any left by a prior engine in this process.
    get_midi_clock_out().reset();
    mMidiControl.init(&mEgress, &mSuperClock);
    mIngress.registerRoute("/midi/", &SupersonicEngine::nrtForwardSink, this);
    mControlIngress.registerRoute("/midi/", &SupersonicEngine::routeTo<MidiControl, &MidiControl::handleMidiCommand>, &mMidiControl);
#endif
#ifdef SUPERSONIC_GAMEPAD
    mGamepadControl.init(&mEgress);
    mIngress.registerRoute("/gamepad/", &SupersonicEngine::nrtForwardSink, this);
    mControlIngress.registerRoute("/gamepad/", &SupersonicEngine::routeTo<GamepadControl, &GamepadControl::handleGamepadCommand>, &mGamepadControl);
#endif
#ifdef SUPERSONIC_OSC
    // The OSC cue server + outbound user OSC. All "/osc/" verbs are control
    // (forwarded to the NRT thread); outbound user OSC is "/osc/send"
    // (immediate, or wrapped in /schedule for timed sends).
    mOscControl.init(&mEgress);
    mIngress.registerRoute("/osc/", &SupersonicEngine::nrtForwardSink, this);
    mControlIngress.registerRoute("/osc/", &SupersonicEngine::routeTo<OscControl, &OscControl::handleOscCommand>, &mOscControl);
#endif
    // "/schedule" is handled in the shared drain (audio_processor.cpp), beside
    // timestamped-bundle scheduling, so it works identically on every target — it
    // never reaches this dispatcher. Only the flush (cancel pending) is wired here.
    mIngress.registerRoute("/sched/flush", &SupersonicEngine::schedFlushSink, this);

    // Single-engine-per-process: g_active_superclock has no per-engine
    // routing, so a second publisher would steer /superclock_get to
    // whichever was last initialised. CAS to detect; warn and overwrite.
    {
        SuperClock* expected = nullptr;
        if (!g_active_superclock.compare_exchange_strong(
                expected, &mSuperClock, std::memory_order_release)) {
            fprintf(stderr,
                "[supersonic] WARNING: another SupersonicEngine has already "
                "published a SuperClock; multi-engine native is not "
                "supported. /superclock_get will reflect this engine.\n");
            fflush(stderr);
            g_active_superclock.store(&mSuperClock, std::memory_order_release);
        }
    }

    // Publish the ingress for the audio-thread drain to classify through (mirrors
    // g_active_superclock; single publisher per process).
    {
        OscIngress* expected = nullptr;
        if (!g_active_ingress.compare_exchange_strong(
                expected, &mIngress, std::memory_order_release)) {
            g_active_ingress.store(&mIngress, std::memory_order_release);
        }
    }

    // NRT gateway drain #2: the control ring. Thread the origin token to the
    // handler as call metadata (the egress resolves it via the transport at reply
    // time — no address touches the engine) and route the command by address to
    // its subsystem handler, off the audio thread. The control dispatcher's
    // default (handleLinkCommand) takes /clock/.
    mNrtGateway.addDrain(
        mNrtBuffer, kNrtRingSize, &mNrtHead, &mNrtTail,
        [this](uint32_t token, const uint8_t* d, uint32_t n, uint32_t) {
            DrainCallCtx cc{ token };   // thread the origin to the handler as metadata
            // Remember the address across the call so a pass that blocks can be
            // reported by name, not just duration.
            noteInFlightCommand(d, n);
            mControlIngress.ingest(d, n, &cc);
            mInFlightCommand[0] = '\0';
        },
        {});

    // Report a control pass that blocked the gateway. Everything queued behind
    // it — later commands and the egress drains below — waited this long, so a
    // client's unanswered request is explained here rather than inferred from a
    // silence in the log.
    mNrtGateway.onSlowPass([this](uint32_t us) {
        ss_log("[nrt] control drain blocked %.1fs%s%s\n", us / 1'000'000.0,
               mInFlightCommand[0] ? " handling " : "",
               mInFlightCommand[0] ? mInFlightCommand : "");
    });

    // NRT gateway drain #3: the NRT egress lane (NRT-out ring), via the lanes
    // ABI. Off-thread producers (EngineControl replies above, Link/device
    // notifications, SampleLoader debug) frame OSC + a route tag through
    // OscEgress → ss_egress_nrt_write; the gateway is the sole transport
    // caller, so deliver each one. Added after the control drain so a reply
    // framed this wake is sent the same wake.
    mNrtGateway.addTask([this]() {
        ss_egress_nrt_drain(
            [](void* ctx, uint32_t token, uint32_t route,
               const uint8_t* osc, uint32_t len, uint32_t) {
                static_cast<SupersonicEngine*>(ctx)->mEgress.dispatchEgress(
                    token, route, osc, len);
            },
            this, 0 /* drain everything available */);
    });

    // Test hook: close the device before the source decision so
    // startAudioSource() sees no current device and enters the "waiting for
    // audio device" state (the default engine never falls back to a silent
    // driver — see desiredAudioSource).
    if (testForceNoCurrentDeviceAfterInit && mDeviceManager) {
        fprintf(stderr,
                "[supersonic] testForceNoCurrentDeviceAfterInit: closing device "
                "to exercise the no-audio-device path\n");
        fflush(stderr);
        mDeviceManager->closeAudioDevice();
    }

    // -- SampleLoader + audio callback wiring ------------------------------
    // Wire to audio callback so installPendingBuffers() runs on the audio
    // thread. Done before startAudioSource() so the audio thread sees a
    // fully-configured callback the first time it fires.
    mSampleLoader.initialise();
    // Off-thread loader diagnostics ride the NRT-out egress ring.
    mSampleLoader.setDebugSink([this](const char* t, uint32_t n) { mEgress.debug(t, n); });
    mAudioCallback.setSampleLoader(&mSampleLoader);
    mAudioCallback.setSuperClock(&mSuperClock);
    mAudioCallback.onWake = [this]() { purge(); };

    // Wire SuperClock's Link-event callbacks → OSC notify push.
    // Callbacks fire on Link's network thread; the egress serialises socket
    // writes via the transport's send().
    {
        OscEgress* egr = &mEgress;
        std::atomic<bool>* alive = &mLinkCallbacksAlive;
        mSuperClock.setTempoChangedCallback([egr, alive](double bpm) {
            if (!alive->load(std::memory_order_acquire)) return;
            // Tempo slides fire this per-update — cap logging at one
            // line per second so a slide can't flood the log.
            static std::atomic<int64_t> lastLogSec{0};
            const int64_t nowSec = std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
            int64_t prev = lastLogSec.load(std::memory_order_relaxed);
            if (nowSec != prev && lastLogSec.compare_exchange_strong(prev, nowSec)) {
                fprintf(stderr, "[link] tempo -> %.2f bpm\n", bpm);
                fflush(stderr);
            }
            char buf[64];
            osc::OutboundPacketStream s(buf, sizeof(buf));
            s << osc::BeginMessage("/clock/notify/tempo") << bpm << osc::EndMessage;
            egr->broadcastLinkNotify(
                reinterpret_cast<const uint8_t*>(s.Data()),
                static_cast<uint32_t>(s.Size()));
        });
        mSuperClock.setNumPeersChangedCallback([egr, alive](std::size_t n) {
            if (!alive->load(std::memory_order_acquire)) return;
            fprintf(stderr, "[link] peers -> %zu\n", n);
            fflush(stderr);
            char buf[64];
            osc::OutboundPacketStream s(buf, sizeof(buf));
            s << osc::BeginMessage("/clock/notify/peers")
              << static_cast<int32_t>(n) << osc::EndMessage;
            egr->broadcastLinkNotify(
                reinterpret_cast<const uint8_t*>(s.Data()),
                static_cast<uint32_t>(s.Size()));
        });
        mSuperClock.setStartStopChangedCallback([this, egr, alive](bool playing, int64_t t) {
            if (!alive->load(std::memory_order_acquire)) return;
            fprintf(stderr, "[link] transport -> %s\n", playing ? "playing" : "stopped");
            fflush(stderr);
            char buf[64];
            osc::OutboundPacketStream s(buf, sizeof(buf));
            // Timestamp in NTP micros, like every other /clock wire time — the
            // Link-domain micros the callback hands us are per-boot and
            // meaningless to a client.
            s << osc::BeginMessage("/clock/notify/transport")
              << static_cast<int32_t>(playing ? 1 : 0)
              << static_cast<osc::int64>(mSuperClock.linkMicrosToNtpMicros(t))
              << osc::EndMessage;
            egr->broadcastLinkNotify(
                reinterpret_cast<const uint8_t*>(s.Data()),
                static_cast<uint32_t>(s.Size()));
        });
    }

    // -- Start worker threads ----------------------------------------------
    // Workers must be running before the audio source starts, otherwise
    // OUT/NRT-out ring buffers can back up during the audio thread's first
    // few hundred ticks (~ms) before any reader is draining them.
    mNrtGateway.start();
    // The gateway now drains NRT-out — off-audio-thread debug (ss_log) routes there
    // instead of the single-writer RT-out ring. Kept true through shutdown's thread
    // teardown so off-thread logging never falls back to racing RT-out; cleared
    // once every egress producer and the audio device are stopped (see shutdown).
    g_nrt_egress_drained.store(true, std::memory_order_relaxed);
    mSampleLoader.startThread(juce::Thread::Priority::normal);

    // -- Start the audio source (real callback or headless fallback) -------
    // Picks based on whether the device manager has a current device.
    // Blocks until process_audio has ticked at least once, or 5 s with a
    // warning. After this returns the engine is fully responsive: OSC sent
    // via sendOSC() / UDP will be drained on the next audio block.
    startAudioSource();

#ifdef __APPLE__
    // CoreAudio default-output listener: JUCE only watches device
    // connect/disconnect, not "user changed default in System Settings".
    // Only install on a real device; a headless-fallback engine has no
    // system default to track. Skipped under the factory seam — fake
    // devices have no HAL presence.
    if (!cfg.deviceManagerFactory &&
        mActiveSource.load() == AudioSource::RealCallback &&
        !mDefaultDevicePropertyListenerInstalled) {
        AudioObjectPropertyAddress pa = {
            kAudioHardwarePropertyDefaultOutputDevice,
            kAudioObjectPropertyScopeGlobal,
            kAudioObjectPropertyElementMain
        };
        if (AudioObjectAddPropertyListener(kAudioObjectSystemObject, &pa,
                                           &SupersonicEngine::defaultDevicePropertyListenerProc,
                                           this) == noErr) {
            mDefaultDevicePropertyListenerInstalled = true;
        }
    }
#endif

    if (testInitFailure) {
        auto msg = testInitFailure();
        if (!msg.empty()) throw std::runtime_error(msg);
    }

    mRunning.store(true);
    setEngineState(EngineState::Running, "boot");

    // Callback-starvation watchdog (see watchdogLoop). Pointless in manual-
    // pump mode, where the test owns process_audio and long gaps are normal.
    if (mCurrentConfig.callbackWatchdog && !mCurrentConfig.manualAudioPump) {
        mWatchdogStop.store(false);
        mWatchdogThread = std::thread(&SupersonicEngine::watchdogLoop, this);
    }
}


void SupersonicEngine::shutdown() {
    // Don't early-out on !mRunning here: a partial init() (which
    // throws before mRunning becomes true) still needs the cleanup below
    // to run, particularly the macOS CoreAudio property listener removal,
    // which would otherwise fire against a destroyed `this`. Each cleanup
    // step below is individually guarded against missing resources.
    bool wasRunning = mRunning.exchange(false);
    if (wasRunning)
        setEngineState(EngineState::Stopped, "shutdown");

    // Join the SuperClock session worker first. It drives MIDI-staleness through
    // the clock (~every 250 ms) and reaches the SHM arena the clock state binds
    // into plus the Link Audio bus — both freed below (mShmemCreator.reset / the
    // setLinkVisibility(Off) teardown). The worker is otherwise only joined when
    // mSuperClock is destroyed, which runs *after* this returns, leaving a window
    // where it runs against freed state. Same early-stop discipline as the MIDI /
    // OSC / gamepad / device threads below.
    mSuperClock.stopBackgroundWork();

    // Stop recording if active
    if (isRecording())
        stopRecording();

    // Stop the NRT gateway before the subsystems its drains call into: the
    // control drain routes /midi/ /gamepad/ /osc/ commands straight to the
    // Rust FFI seams, so the reader must be joined before any ss_*_destroy —
    // an ss_midi_handle_osc still on the gateway thread while ss_midi_destroy
    // frees the SsMidi it reads is a use-after-free. The gateway also runs
    // EngineControl, whose scheduleDeviceSwitch posts to the device task
    // lane; joining that lane below while the gateway could still post to
    // it would drop work. stop() bumps + notifies its wake word (processCount) and
    // joins. Late notifications the subsystems emit after this point sit
    // undrained in the NRT-out ring — acceptable, the consumer is going away.
    mNrtGateway.stop();

    // Tear down the MIDI subsystem before mEgress/mSuperClock: ss_midi_destroy
    // closes its midir connections (stopping midir's input thread), so no MIDI
    // callback can fire into mEgress/mSuperClock after this returns.
#ifdef SUPERSONIC_MIDI
    mMidiControl.shutdown();
    // Drop clock-out state so a generated clock can't resume with stale ports on
    // the next boot (the coordinator is a process-wide singleton).
    get_midi_clock_out().reset();
#endif

    // Tear down the gamepad subsystem before mEgress for the same reason:
    // ss_gamepad_destroy deregisters the host callback (synchronising on the
    // emission lock), so no gamepad callback can fire into mEgress after this
    // returns. The subsystem's poll thread itself is process-global and parks.
#ifdef SUPERSONIC_GAMEPAD
    mGamepadControl.shutdown();
#endif

    // Order: signal alive=false, then disable Link (this joins the
    // network thread synchronously inside link.enable(false), so
    // in-flight callbacks complete), then clear the callback wrappers.
    // Clearing earlier leaves the wrapper closure installed during the
    // brief window before Link quiesces.
    mLinkCallbacksAlive.store(false, std::memory_order_release);
    mSuperClock.setLinkVisibility(SuperClock::LinkVisibility::Off);
    mSuperClock.setTempoChangedCallback({});
    mSuperClock.setNumPeersChangedCallback({});
    mSuperClock.setStartStopChangedCallback({});

    // Tear down the OSC cue server (joins its recv thread, closes the socket).
    // The gateway reader is already joined above: that reader drains inbound
    // commands through mControlIngress into OscControl::handleOscCommand, which
    // reads mOsc — destroying mOsc (ss_osc_destroy → mOsc=nullptr) under a
    // still-running reader would be a data race. Still before mEgress, so no
    // inbound cue can fire into mEgress after this returns.
#ifdef SUPERSONIC_OSC
    mOscControl.shutdown();
#endif

    // Stop the watchdog before tearing down — it can launch a recovery
    // (requestAudioRecovery) right up until it observes mWatchdogStop. The NRT
    // gateway (the other requester) is already stopped above, so once the
    // watchdog joins no new recovery can start. The device task lane (joined below) runs recovery; stop the watchdog before
    // teardownDeviceManager frees what it operates on: a recovery that already
    // passed its mRunning check runs to completion and is waited on here.
    mWatchdogStop.store(true);
    if (mWatchdogThread.joinable()) mWatchdogThread.join();

    // No control command can run now — join the device-orchestration worker
    // before tearing down the device manager + egress it calls into.
    mDebounceSwitchStop.store(true);

    // Same window as the debounce worker: device tasks touch the device manager
    // and egress, both torn down below.
    mDeviceTaskStop.store(true);
    mDeviceTaskCv.notify_all();
    if (mDeviceTaskThread.joinable()) mDeviceTaskThread.join();

    mHeadlessDriver.signalThreadShouldExit();
    mSampleLoader.signalThreadShouldExit();

    // Wake the readers so they can exit. Both wait on processCount; bump it so
    // wait() sees a change and returns.
    mAudioCallback.processCount.fetch_add(1, std::memory_order_release);
    mAudioCallback.processCount.notify_all();

    // Wake SampleLoader's WaitableEvent so it can see threadShouldExit
    mSampleLoader.wake();

    teardownDeviceManager();

#ifdef __APPLE__
    if (mDefaultDevicePropertyListenerInstalled) {
        AudioObjectPropertyAddress pa = {
            kAudioHardwarePropertyDefaultOutputDevice,
            kAudioObjectPropertyScopeGlobal,
            kAudioObjectPropertyElementMain
        };
        AudioObjectRemovePropertyListener(
            kAudioObjectSystemObject, &pa,
            &SupersonicEngine::defaultDevicePropertyListenerProc,
            this);
        mDefaultDevicePropertyListenerInstalled = false;
    }
    AggregateDeviceHelper::destroy();
#endif

    mHeadlessDriver.stopThread(2000);
    mSampleLoader.stopThread(2000);

    // Unpublish from /superclock_get only if we're the current
    // publisher — never stomp another engine's pointer.
    {
        SuperClock* expected = &mSuperClock;
        g_active_superclock.compare_exchange_strong(
            expected, nullptr, std::memory_order_release);
    }

    // Unpublish the audio-thread ingress (same single-publisher discipline).
    {
        OscIngress* expected = &mIngress;
        g_active_ingress.compare_exchange_strong(
            expected, nullptr, std::memory_order_release);
    }

    // NRT-out is no longer drained (gateway stopped above) and every egress
    // producer plus the audio device are now stopped, so off-thread debug can
    // safely fall back to RT-out again — and the flag is reset for the next engine.
    g_nrt_egress_drained.store(false, std::memory_order_relaxed);

    // Tear down the World and the engine's global arena view while the
    // segment is still mapped — after this the lanes entry points reject,
    // so nothing can dereference the arena once it is unmapped below.
    teardown_memory();

    // Destroy engine-owned shared memory (after the World is gone). The peer
    // plane lives in the segment: null the published slot first so a late
    // ShmTransport send observes null rather than an unmapped plane. Same for
    // the SuperClock sample-clock binding — a late pumpAudioBlock must
    // publish into nothing rather than the unmapped segment.
    mSuperClock.bindSampleClockToShm(nullptr);
    mPeerPlane.store(nullptr, std::memory_order_release);
    g_external_shared_memory = nullptr;
    g_external_segment = nullptr;
    mShmemCreator.reset();
}

// --- OSC send with cache interception ---

void SupersonicEngine::sendOSC(const uint8_t* data, uint32_t size) {
    if (size >= 8 && data[0] == '/') {
        interceptForCache(data, size);
    }
    // The in-process entry point: origin 0 = an anonymous in-process caller,
    // assigned explicitly here (every entry point mints its own origin — UDP
    // interns the sender on recv).
    ingest(data, size, 0);
}

void SupersonicEngine::pumpAudioBlock() {
    // Anchor the audio-thread clock on the first manual block (mirrors what
    // HeadlessDriver::run does at thread start). Safe to call after stopping the
    // HeadlessDriver — picks up rendering from a clean time base.
    if (!mManualPumpStarted) {
        mManualSamplePos = 0.0;
        mSuperClock.resetAudioThreadTime(mManualSamplePos, mCurrentConfig.sampleRate);
        mManualPumpStarted = true;
    }

    mSampleLoader.installPendingBuffers();

    const double   ntp        = mSuperClock.updateAudioThreadNTP(
                                    mManualSamplePos, mCurrentConfig.sampleRate);
    const uint64_t hostMicros = static_cast<uint64_t>(
                                    std::max<int64_t>(0, mSuperClock.linkClockMicros()));

    // scsynth's actual block size is authoritative for bus striding; the config
    // buffer size can differ from the render quantum.
    const uint32_t blockSize = static_cast<uint32_t>(get_audio_buffer_samples());
    const uint32_t nOut = mCurrentConfig.numOutputChannels > 0
                              ? static_cast<uint32_t>(mCurrentConfig.numOutputChannels) : 2;
    const uint32_t nIn  = mCurrentConfig.numInputChannels  > 0
                              ? static_cast<uint32_t>(mCurrentConfig.numInputChannels)  : 0;

    // Sample clock: the manual pump renders with no live device (idle /
    // device-loss), so "audible" == render time (latency 0).
    mSuperClock.publishSampleClock(mManualSamplePos,
                                   static_cast<double>(mCurrentConfig.sampleRate),
                                   ntp, 0);

    renderAudioBlock(mSuperClock, blockSize, nOut, nIn,
                     static_cast<uint32_t>(mCurrentConfig.sampleRate), ntp, hostMicros);
    mManualSamplePos += blockSize;

    mAudioCallback.processCount.fetch_add(1, std::memory_order_release);
    mAudioCallback.processCount.notify_all();
}

// Copy the OSC address of the command about to be handled. Bounded copy off the
// raw packet: an address is NUL-terminated at the head of the message, so no
// decode is needed on this path.
void SupersonicEngine::noteInFlightCommand(const uint8_t* data, uint32_t size) {
    mInFlightCommand[0] = '\0';
    if (!data || size == 0 || data[0] != '/') return;
    const uint32_t max = std::min<uint32_t>(size, sizeof(mInFlightCommand) - 1);
    uint32_t i = 0;
    for (; i < max && data[i] != '\0'; ++i)
        mInFlightCommand[i] = static_cast<char>(data[i]);
    mInFlightCommand[i] = '\0';
}

void SupersonicEngine::ingest(const uint8_t* data, uint32_t size, uint32_t originToken) {
    // Dumb transport: write the bytes onto the ingress lane (the IN ring) with
    // the opaque origin token in the Message header. The audio thread drains,
    // classifies (OscIngress), and either performs the audio plane inline or
    // forwards control to the NRT thread — which resolves the token back to a
    // reply address via the transport. Token 0 (in-process / embedder) replies
    // via onReply.
    bool written = ss_ingress_write(data, size, originToken);
    if (mMetrics) {
        if (written) {
            mMetrics->osc_out_messages_sent.fetch_add(1, std::memory_order_relaxed);
            mMetrics->osc_out_bytes_sent.fetch_add(size, std::memory_order_relaxed);
        } else {
            // Backpressure / oversize frame: the message is gone — count it
            // as a drop, never as sent.
            mMetrics->messages_dropped.fetch_add(1, std::memory_order_relaxed);
        }
    }
}

// --- Audio-thread control route: forward /clock + /supersonic to the NRT ring -
// Runs on the audio thread (the OscIngress default-less route). Writes the raw
// control message to the process-local NRT command ring with the origin token in
// the Message header; the NrtCommandReader thread drains + runs EngineControl.
bool SupersonicEngine::nrtForwardSink(void* ctx, const void* callCtx,
                                      const uint8_t* data, std::size_t len) {
    auto* self = static_cast<SupersonicEngine*>(ctx);
    auto* cc   = static_cast<const DrainCallCtx*>(callCtx);
    bool ok = RingBufferWriter::write(
        self->mNrtBuffer, kNrtRingSize,
        &self->mNrtHead, &self->mNrtTail, &self->mNrtSeq, &self->mNrtLock,
        data, static_cast<uint32_t>(len), cc ? cc->sourceId : 0);
    // The NRT gateway drains this ring every audio block (it waits on
    // processCount), so no wake is needed here. A full ring drops the control
    // message — count it rather than losing it silently (sender gets no reply).
    if (!ok && self->mMetrics)
        self->mMetrics->messages_dropped.fetch_add(1, std::memory_order_relaxed);
    return true;  // consumed — control never falls through to scsynth
}

bool SupersonicEngine::schedFlushSink(void* /*ctx*/, const void* /*callCtx*/,
                                      const uint8_t* data, std::size_t len) {
    // /sched/flush <tag> — drop pending scheduled events with this tag. An
    // empty/missing tag defaults to the user-scheduled tag (not the wildcard),
    // so a tagless flush can never wipe pending synth bundles or the clock.
    // Runs on the audio thread, same as enqueue/tick.
    uint32_t tag = SCHED_TAG_DEFAULT;
    try {
        osc::ReceivedMessage msg(osc::ReceivedPacket(
            reinterpret_cast<const char*>(data),
            static_cast<osc::osc_bundle_element_size_t>(len)));
        auto it = msg.ArgumentsBegin();
        if (it != msg.ArgumentsEnd() && it->IsString()) {
            const char* t = it->AsStringUnchecked();
            if (t && *t) tag = sched_tag_hash(t, std::strlen(t));
        }
    } catch (...) {
        return true;
    }
    get_scheduler().flush(tag);
    return true;
}

// --- Device switch / reopen orchestration -------------------------------------

void SupersonicEngine::postDeviceTask(std::function<void()> task) {
    if (!task) return;
    {
        std::lock_guard<std::mutex> lock(mDeviceTaskMutex);
        if (mDeviceTaskStop.load()) return;   // shutting down: drop rather than queue
        mDeviceTasks.push_back(std::move(task));
        if (!mDeviceTaskThread.joinable())
            mDeviceTaskThread = std::thread(&SupersonicEngine::deviceTaskLoop, this);
    }
    mDeviceTaskCv.notify_one();
}

void SupersonicEngine::deviceTaskLoop() {
    for (;;) {
        std::function<void()> task;
        {
            std::unique_lock<std::mutex> lock(mDeviceTaskMutex);
            mDeviceTaskCv.wait(lock, [this] {
                return mDeviceTaskStop.load() || !mDeviceTasks.empty();
            });
            // Drain what is already queued before exiting, so work accepted
            // just before shutdown still completes (and still replies).
            if (mDeviceTasks.empty()) return;
            task = std::move(mDeviceTasks.front());
            mDeviceTasks.pop_front();
        }
        task();
    }
}

void SupersonicEngine::scheduleDeviceSwitch(const std::string& devName,
                                            const std::string& inputDevName,
                                            double sampleRate, int bufferSize) {
    // Rapid clicks replace the pending switch; only the last one executes
    // after the debounce worker's quiet period.
    {
        std::lock_guard<std::mutex> lock(mPendingSwitchMutex);
        mPendingSwitch.devName = devName;
        mPendingSwitch.inputDevName = inputDevName;
        mPendingSwitch.sampleRate = sampleRate;
        mPendingSwitch.bufferSize = bufferSize;
        mPendingSwitch.timestamp = std::chrono::steady_clock::now();
        mPendingSwitch.active = true;
    }
    if (!mDebounceSwitchRunning.load()) {
        mDebounceSwitchRunning.store(true);
        postDeviceTask([this]() { executePendingSwitch(); });
    }
}

void SupersonicEngine::executePendingSwitch() {
    // Wait for rapid clicks to settle (500ms quiet period)
    constexpr auto kDebounceMs = std::chrono::milliseconds(500);

    std::string devName, inputDevName;
    double sr = 0;
    int bufSz = 0;

    while (!mDebounceSwitchStop.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        if (mDebounceSwitchStop.load()) break;

        std::lock_guard<std::mutex> lock(mPendingSwitchMutex);
        if (!mPendingSwitch.active) {
            mDebounceSwitchRunning.store(false);
            return;
        }

        auto elapsed = std::chrono::steady_clock::now() - mPendingSwitch.timestamp;
        if (elapsed >= kDebounceMs) {
            devName      = mPendingSwitch.devName;
            inputDevName = mPendingSwitch.inputDevName;
            sr           = mPendingSwitch.sampleRate;
            bufSz        = mPendingSwitch.bufferSize;
            mPendingSwitch.active = false;
            break;
        }
    }

    if (mDebounceSwitchStop.load()) {
        mDebounceSwitchRunning.store(false);
        return;
    }

    fprintf(stderr, "[device-setup] debounced switch: out='%s' in='%s' sr=%.0f buf=%d\n",
            devName.c_str(), inputDevName.c_str(), sr, bufSz);
    fflush(stderr);

    // Explicit GUI switch → force device mode so changeListenerCallback
    // doesn't fight us by reinitialising to system defaults.
    if (!devName.empty())
        forceDeviceMode(devName);

    // Retry if a swap is already in progress (e.g. cascade from a
    // device-change notification). Give it up to ~3 seconds.
    SwapResult result;
    int attempts = 0;
    for (int attempt = 0; attempt < 30; ++attempt) {
        attempts = attempt + 1;
        result = switchDevice(devName, sr, bufSz, false, inputDevName);
        if (result.success || result.error != "swap already in progress") break;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    if (!result.success) {
        fprintf(stderr, "[device-setup] debounced switch failed after %d attempts: %s\n",
                attempts, result.error.c_str());
        fflush(stderr);
    }
    // No sendDeviceReport() here — switchDevice's printDeviceList already
    // broadcasts. A second call can race with JUCE's post-switch device-list
    // rescan and report an empty snapshot. Push the truthful outcome instead.
    sendSwitchDone(result, devName, inputDevName);

    mDebounceSwitchRunning.store(false);
}

bool SupersonicEngine::tryAcquireSwapGate(std::unique_lock<std::recursive_mutex>& lk,
                                          int attempts, int sleepMs) {
    lk = std::unique_lock<std::recursive_mutex>(mSwapMutex, std::defer_lock);
    for (int i = 0; i < attempts; ++i) {
        if (lk.try_lock()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(sleepMs));
    }
    return false;
}

SupersonicEngine::TestSwapHold SupersonicEngine::testHoldSwapGate() {
    return TestSwapHold(mSwapMutex, mDevicePhase);
}

// Callback-starvation watchdog. The audio callback is the sole drain for
// synth commands (process_audio consumes the IN ring), so a device whose
// callback thread wedges — e.g. DirectSound spinning in its cursor poll
// after a mid-reconfigure race — leaves the server deaf forever while its
// control socket stays up. Sample processCount; when it freezes past the
// stall window with a source nominally active and no swap/reopen in
// flight, restart the source (headless) or request a device reopen (real).
void SupersonicEngine::watchdogLoop() {
    const int64_t stallMs = std::max(1, mCurrentConfig.watchdogStallMs);
    const int     pollMs  = std::max(10, mCurrentConfig.watchdogPollMs);

    // Liveness by SUSTAINED ticks: a lone tick (one callback per failed attempt)
    // reads as Confirming, not Live, so it can't masquerade as recovered. Ticks
    // must sustain for a stall window to count as live.
    sonicpi::audio::LivenessMonitor liveness(stallMs, stallMs);

    // Rate skew by rendered frames vs a monotonic clock: a device can keep
    // ticking (Live) while its timer free-runs fast or slow (post-sleep
    // DirectSound), splitting the audio timebase seconds away from the wall
    // clock. maxGap of a few polls makes any sampling pause (benign skip, swap,
    // machine sleep) discard the window instead of reading as skew.
    const bool rateCheck = mCurrentConfig.watchdogRateWindowMs > 0;
    sonicpi::audio::RateSkewMonitor rateSkew(
        mCurrentConfig.watchdogRateWindowMs,
        /*maxGap*/ std::max<int64_t>(3 * pollMs, 1000),
        mCurrentConfig.watchdogRateTolerance,
        std::max(1, mCurrentConfig.watchdogRateBadWindows));
    // Detection is logged once per skew episode, separately from the recovery
    // action: requestAudioRecovery can be gated (cooldown / already in flight)
    // for many polls, and a silent gated detection would leave user logs with
    // no trace of WHY a later recovery fired.
    bool rateSkewLogged = false;

    auto nowMs = [] {
        return (int64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
    };

    while (!mWatchdogStop.load()) {
        // Sleep in small slices so shutdown joins quickly.
        for (int slept = 0; slept < pollMs && !mWatchdogStop.load(); slept += 20)
            std::this_thread::sleep_for(
                std::chrono::milliseconds(std::min(20, pollMs - slept)));
        if (mWatchdogStop.load()) return;

        // Publish control-thread blocking from here, off the gateway: a gateway
        // stuck in a handler cannot report its own stall.
        // µs, not ms: healthy gateway passes are tens of µs, so integer ms
        // rounds every reading a user ever sees to 0.
        World_PublishNrtBlocking(mNrtGateway.maxPassUs(),
                                 mNrtGateway.recentMaxPassUs(),
                                 mNrtGateway.inFlightUs());

        // Waiting for an audio device (no device open, not a headless / manual-
        // pump build). Keep trying to open one so the engine self-heals the
        // moment a device appears (plug in / wake). requestAudioRecovery is
        // cooldown-gated.
        if (waitingForAudioDevice() && !mReopenInProgress.load()) {
            // Skip while a swap holds the gate — the same guard the benign block
            // below applies. A normal switchDevice passes through a transient
            // mActiveSource==None window with the gate held and will bring a
            // device up itself; racing a recovery into that window would, on
            // winning the gate, recreate the device manager and revert to the
            // system default — silently undoing the switch. The next poll retries
            // once the gate frees.
            const bool swapInFlight =
                mDevicePhase.load() != DevicePhase::Idle;
            if (!swapInFlight) {
                std::string reason;
                if (requestAudioRecovery(reason)) {
                    mWatchdogRecoveries.fetch_add(1, std::memory_order_release);
                    ss_log("[watchdog] no audio device — attempting to open one");
                }
            }
            continue;
        }

        // States where a gap in ticks is expected, not a stall: engine not
        // running, no source active (manual pump / mid-transition), a recovery
        // already in flight, or a swap holding the gate. Skip sampling entirely
        // so a legitimately-paused callback can't drift the monitor to a false
        // stall.
        bool benign = !mRunning.load()
                   || mActiveSource.load() == AudioSource::None
                   || mReopenInProgress.load();
        if (!benign && mDevicePhase.load() != DevicePhase::Idle)
            benign = true;  // mutation in flight — callbacks legitimately paused
        if (benign) continue;

        const int64_t  t     = nowMs();
        const uint32_t count = mAudioCallback.processCount.load(std::memory_order_acquire);
        liveness.observe(count, t);
        const auto ph = liveness.phase(t);

        if (mActiveSource.load() == AudioSource::Headless) {
            // Explicit-headless build (tests / non-JUCE backends). If its timer
            // thread died/wedged, restart it under the swap gate; otherwise it's
            // ticking fine and there's no real device to recover.
            if (ph == sonicpi::audio::LivenessPhase::Stalled) {
                std::unique_lock<std::recursive_mutex> lk;
                if (tryAcquireSwapGate(lk, 1, 0)
                    && mActiveSource.load() == AudioSource::Headless) {
                    mWatchdogRecoveries.fetch_add(1, std::memory_order_release);
                    ss_log("[watchdog] headless driver stalled — restarting");
                    stopAudioSource();
                    startAudioSource();
                }
            }
            continue;
        }

        // Real device source.
        if (ph != sonicpi::audio::LivenessPhase::Stalled) {
            // Ticking — but at the right rate? Only judge while Live (a
            // Confirming run is still settling) and not paused (stopRecording
            // pauses the callback briefly: ticks continue, frames freeze — a
            // false skew).
            if (rateCheck && ph == sonicpi::audio::LivenessPhase::Live
                && !mAudioCallback.isPaused()) {
                const int nominal = mAudioCallback.nominalSampleRate();
                rateSkew.observe(mSuperClock.engineFrames(),
                                 static_cast<double>(nominal) / 1000.0, t);
                if (rateSkew.skewed()) {
                    if (!rateSkewLogged) {
                        rateSkewLogged = true;
                        ss_log("[watchdog] rate skew detected: device delivering "
                               "%.2fx real-time (nominal %d Hz) — clock cannot "
                               "converge, will recover with a cold swap",
                               rateSkew.lastRatio(), nominal);
                    }
                    std::string reason;
                    if (requestAudioRecovery(reason)) {
                        mWatchdogRecoveries.fetch_add(1, std::memory_order_release);
                        mRateSkewRecoveries.fetch_add(1, std::memory_order_release);
                        ss_log("[watchdog] rate skew: recovering with a cold swap "
                               "(%.2fx real-time)", rateSkew.lastRatio());
                        rateSkew.reset();
                        rateSkewLogged = false;
                    }
                } else {
                    rateSkewLogged = false;
                }
            }
            continue;  // Live (healthy) or Confirming (give the ticks the window)
        }

        // The device stopped delivering. Recover with a cold swap on a fresh
        // connection. requestAudioRecovery is in-flight/cooldown gated, so only
        // log (and count) when one actually starts — not on every poll of the
        // stall.
        std::string reason;
        if (requestAudioRecovery(reason)) {
            mWatchdogRecoveries.fetch_add(1, std::memory_order_release);
            ss_log("[watchdog] audio device stalled (processCount=%u) — recovering", count);
        }
    }
}

bool SupersonicEngine::requestAudioRecovery(std::string& reason) {
    // Space recoveries: the cooldown covers Spider's cold_swap_reinit after a
    // successful promotion (reloads synthdefs / groups / mixer / scope, ~1-3 s);
    // a tighter cooldown would race a still-running reinit.
    constexpr int kRecoveryCooldownMs = 3000;

    // Claim the in-flight slot atomically: the watchdog and the /reopen control
    // thread can both reach here, and a load-then-store would let both pass and
    // launch two recoveries back-to-back (the second interrupting the first's
    // cold_swap_reinit — exactly what the cooldown exists to prevent).
    bool expected = false;
    if (!mReopenInProgress.compare_exchange_strong(expected, true)) {
        reason = "already in progress";
        return false;
    }

    const int64_t now = (int64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    const int64_t last = mLastReopenFinishedAtMs.load();
    const int64_t sinceLast = last ? now - last : kRecoveryCooldownMs;
    if (sinceLast < kRecoveryCooldownMs) {
        mReopenInProgress.store(false);   // release the slot we just claimed
        char msg[96];
        snprintf(msg, sizeof(msg), "cooldown (%lld ms since last)",
                 (long long)sinceLast);
        reason = msg;
        return false;
    }

    // Run on the device task lane (not the JUCE message thread): the Linux
    // standalone loop never pumps that queue, so a posted recovery would never
    // run there — and off the message thread the multi-second swap can't starve
    // it anywhere. The lane serialises recovery against every other deferred
    // device mutation; recoverAudio additionally holds the swap gate, which
    // the message-thread device handlers (changeListenerCallback) respect.
    postDeviceTask([this]() { recoverAudio(); });
    reason = "started";
    return true;
}

void SupersonicEngine::recoverAudio() {
    // A recovery can still be queued/starting when shutdown begins; skip rather
    // than operate on a torn-down engine. shutdown() sets mRunning=false and
    // then joins the device task lane: a recovery already past this check
    // completes and is waited on, while one that hasn't started yet
    // early-returns here.
    if (!mRunning.load()) {
        mReopenInProgress.store(false);
        return;
    }
    PhaseGuard phase(mDevicePhase, DevicePhase::Recovering);

    // Recovery is a full cold swap on a FRESH connection:
    //   1. recreateDeviceManager — a reopen reuses the hibernate-dead CoreAudio
    //      connection; only a brand-new AudioDeviceManager gets a live,
    //      coreaudiod-driven IO thread.
    //   2. reopenCurrentDevice — the normal cold swap on that fresh manager:
    //      it rebuilds the scsynth World and emits /supersonic/setup, which makes
    //      Spider stop the now-dead jobs (the pre-outage timeline is closed by
    //      hibernation) and rebuild its groups to match — so a fresh Run works
    //      with no manual Stop, and there's no World/Spider mismatch.
    // Hold the swap gate across the whole swap so recreateDeviceManager's
    // mDeviceManager reset() is serialised against the device readers
    // (currentDevice/listDevices/sendDeviceReport, which now take the gate) and
    // any other swap. The gate is recursive, so reopenCurrentDevice re-taking it
    // is fine; and it must be released before the reopen.done broadcast below
    // (sendDeviceReport re-takes it) — hence the inner scope.
    juce::String err;
    bool restored = false;
    SwapResult swap;   // device name/rate/buffer from the cold swap — reused below
    {
        std::unique_lock<std::recursive_mutex> lk;
        if (!tryAcquireSwapGate(lk, 20, 50)) {
            // Another thread genuinely holds the gate (a swap in flight) — it will
            // re-establish audio. Tell any /reopen caller it didn't run (so it
            // isn't left waiting), but DON'T stamp the cooldown: no recovery
            // happened, so the watchdog must stay free to retry the moment the
            // gate frees rather than sitting out a 3 s window for nothing.
            broadcastReopenDone(false, {}, 0, 0,
                                "device busy — another swap in progress");
            mReopenInProgress.store(false);
            return;
        }
        try {
            stopAudioSource();                      // detach the dead callback
            err = recreateDeviceManager();
            if (err.isEmpty()) swap = reopenCurrentDevice();
            restored = err.isEmpty() && swap.success
                    && mActiveSource.load() == AudioSource::RealCallback;
        } catch (const std::exception& ex) {
            err = juce::String("recovery exception: ") + ex.what();
        } catch (...) {
            err = "recovery exception (unknown)";
        }

        // If the swap couldn't bring a source up (recreate opened no device, or
        // an exception aborted mid-swap), run startAudioSource to settle into the
        // right idle state: an explicit-headless build restarts its driver; the
        // default build enters the "waiting for audio device" state, and the
        // watchdog keeps retrying until a device appears.
        if (mActiveSource.load() == AudioSource::None) {
            try { startAudioSource(); } catch (...) {}
        }
    }   // release the gate before sendDeviceReport (which re-takes it)

    ss_log("[recover] %s (err='%s')",
           restored ? "restored real audio" : "device down — watchdog will retry",
           err.toRawUTF8());

    // Report to Spider/GUI over the reopen.done channel. On success Spider runs
    // its cold-swap reinit; otherwise it's an informational failure. Report the
    // fields the cold swap already resolved (single source of truth, and no
    // ungated mDeviceManager read out here).
    if (restored)
        broadcastReopenDone(true, swap.deviceName, swap.sampleRate, swap.bufferSize, {});
    else
        broadcastReopenDone(false, {}, 0, 0,
                            err.isEmpty() ? "audio device not yet recovered"
                                          : err.toStdString());

    if (restored) sendDeviceReport();

    // ALWAYS clear the in-flight flag + stamp the cooldown, even after an
    // exception above — otherwise mReopenInProgress leaks true and the watchdog
    // (which treats it as benign) never recovers again for the rest of the session.
    mLastReopenFinishedAtMs.store(
        (int64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
    mReopenInProgress.store(false);
}

void SupersonicEngine::broadcastReopenDone(bool success, const std::string& deviceName,
                                           double sampleRate, int bufferSize,
                                           const std::string& error) {
    char buf[1024];
    osc::OutboundPacketStream s(buf, sizeof(buf));
    s << osc::BeginMessage("/supersonic/devices/reopen.done")
      << static_cast<osc::int32>(success ? 1 : 0)
      << deviceName.c_str()
      << static_cast<float>(sampleRate)
      << static_cast<osc::int32>(bufferSize)
      << (error.empty() ? "" : error.c_str())
      << osc::EndMessage;
    mEgress.broadcastToTargets(reinterpret_cast<const uint8_t*>(s.Data()),
                               static_cast<uint32_t>(s.Size()));
}

void SupersonicEngine::sendSwitchDone(const SwapResult& result,
                                      const std::string& requestedOutput,
                                      const std::string& requestedInput) {
    if (!mEgress.hasSubscribers()) return;

    // Wire format (all fields present on every emit):
    //   success(int32),
    //   requestedOutput(str),  requestedInput(str),
    //   actualOutput(str),     actualInput(str),
    //   error(str),                  ← top-level error, "" on success
    //   inputUnavailable(int32),     ← 1 = output opened, input fell back
    //   inputUnavailableReason(str)  ← JUCE verbatim, "" otherwise
    char buf[2048];
    osc::OutboundPacketStream s(buf, sizeof(buf));
    s << osc::BeginMessage("/supersonic/devices/switch.done")
      << static_cast<osc::int32>(result.success ? 1 : 0)
      << requestedOutput.c_str()
      << requestedInput.c_str()
      << result.deviceName.c_str()
      << result.inputDeviceName.c_str()
      << result.error.c_str()
      << static_cast<osc::int32>(result.inputUnavailable ? 1 : 0)
      << result.inputUnavailableReason.c_str()
      << osc::EndMessage;
    mEgress.broadcastToTargets(reinterpret_cast<const uint8_t*>(s.Data()),
                               static_cast<uint32_t>(s.Size()));
}

void SupersonicEngine::sendDeviceReport() {
    if (!mEgress.hasSubscribers()) return;
    // Serialise mDeviceManager access against device mutations / recovery's
    // recreate. Recursive: the mutation paths call this while holding the gate.
    std::lock_guard<std::recursive_mutex> gate(mSwapMutex);

    // Skip rescan — sendDeviceReport() is usually called right after a
    // device switch, when rescanning would disrupt the freshly-opened device.
    auto allDevices = listDevices(false);
    auto current = currentDevice();
    auto mode    = deviceMode();

    // All list-shaping (clutter/wireless filters, known-bad inputs,
    // unpairable-output input clearing, per-driver vs deduped-flat split,
    // transient-snapshot suppression) is pure policy — see
    // selectReportedDevices for the contractual filter order.
    std::vector<std::string> knownBadInputs;
    if (!current.name.empty())
        for (auto& dev : allDevices)
            if (dev.maxInputChannels > 0
                && isInputKnownBadFor(current.name, dev.name))
                knownBadInputs.push_back(dev.name);

    auto selection = sonicpi::device::selectReportedDevices(
        allDevices, current.name, current.inputDeviceName,
        current.typeName, knownBadInputs);
    auto& outputDevices = selection.outputs;
    auto& inputDevices  = selection.inputs;

    // Per-driver device table: the same filtered devices, grouped by driver
    // type and NOT deduped — one group per driver, so a client can render
    // any driver's device list directly instead of inferring it from the
    // flat list below, whose dedupe keeps only the active driver's entry
    // for a shared name (Windows: the WASAPI variants and DirectSound
    // expose identical endpoint names). Group order follows enumeration
    // order. Built from the pre-dedupe (per-driver) selection lists.
    struct DriverGroup {
        std::string driver;
        std::vector<std::string> outputs, inputs;
        std::vector<std::string> outputFlags, inputFlags;   // parallel, "" = none
    };
    std::vector<DriverGroup> deviceTable;
    {
        auto groupFor = [&](const std::string& t) -> DriverGroup& {
            for (auto& g : deviceTable)
                if (g.driver == t) return g;
            deviceTable.push_back({t, {}, {}, {}, {}});
            return deviceTable.back();
        };
        for (auto& dev : selection.outputsByDriver)
            groupFor(dev.typeName).outputs.push_back(dev.name);
        for (auto& dev : selection.inputsByDriver)
            groupFor(dev.typeName).inputs.push_back(dev.name);

        // Capability flags make the table the single source of truth for
        // client dropdowns: the GUI renders rows and their semantics from
        // here instead of synthesizing an "OS Default" entry client-side.
        // Drivers without a native default-follow device get a synthetic
        // flagged row; picking it is translated to system mode by
        // EngineControl (see isSyntheticDefaultPick).
#if defined(__linux__) && defined(SUPERSONIC_PIPEWIRE)
        const std::string nativeFollowDriver = "PipeWire";
        const std::string followName = pipeWireDefaultDeviceName();
        const std::string exclusiveName = pipeWirePatchbayDeviceName();
#else
        const std::string nativeFollowDriver;
        const std::string followName = sonicpi::device::kSystemDefaultTableName;
        const std::string exclusiveName;
#endif
        for (auto& g : deviceTable) {
            auto out = sonicpi::device::annotateDriverOutputs(
                g.driver, g.outputs, nativeFollowDriver, followName, exclusiveName);
            g.outputFlags = std::move(out.flags);
            if (out.insertSyntheticDefault) {
                g.outputs.insert(g.outputs.begin(), out.syntheticName);
                g.outputFlags.insert(g.outputFlags.begin(), out.syntheticFlags);
            }
            // Inputs carry the same per-device capabilities but never a
            // synthetic default row — "no input" is already an explicit
            // GUI choice, and input-follow semantics ride on the native
            // driver's own entry.
            auto in = sonicpi::device::annotateDriverOutputs(
                g.driver, g.inputs, nativeFollowDriver, followName, exclusiveName);
            g.inputFlags = std::move(in.flags);
        }
    }

    // The flat lists (selection.outputs/inputs) arrive deduped by name,
    // active driver's entry winning — see selectReportedDevices step 6.
    fprintf(stderr, "[device-list] outputs=%zu inputs=%zu currentIn='%s' currentOut='%s'\n",
            outputDevices.size(), inputDevices.size(),
            current.inputDeviceName.c_str(), current.name.c_str());
    fflush(stderr);
    if (selection.suppressReport) {
        fprintf(stderr, "[device-list] skipping: currentIn set but inputDevices list empty "
                "(transient enumeration, probably mid-swap)\n");
        fflush(stderr);
        return;
    }

    // Build per-driver device table message
    // Format: currentDriver(str), intendedDriver(str), numDrivers(int32),
    //         then per driver: name(str),
    //         numOutputs(int32), then per output: name(str), flags(str),
    //         numInputs(int32),  then per input:  name(str), flags(str).
    //         Flags are comma-separated capability tokens
    //         ("follows-default", "exclusive-duplex", "synthetic"), "" =
    //         plain device. Counts-first throughout so parsers never have
    //         to type-sniff. Overflow degrades to skipping just this
    //         message — clients fall back to the flat report, and
    //         oscpack's throw must never reach mBootDeviceReportThread.
    char tableBuf[8192];
    osc::OutboundPacketStream tableMsg(tableBuf, sizeof(tableBuf));
    bool tableOk = true;
    try {
        tableMsg << osc::BeginMessage("/supersonic/device-table")
                 << currentDriver().c_str()
                 << mIntendedDriver.c_str()
                 << static_cast<osc::int32>(deviceTable.size());
        for (auto& g : deviceTable) {
            tableMsg << g.driver.c_str()
                     << static_cast<osc::int32>(g.outputs.size());
            for (size_t i = 0; i < g.outputs.size(); ++i) {
                tableMsg << g.outputs[i].c_str();
                tableMsg << (i < g.outputFlags.size() ? g.outputFlags[i].c_str() : "");
            }
            tableMsg << static_cast<osc::int32>(g.inputs.size());
            for (size_t i = 0; i < g.inputs.size(); ++i) {
                tableMsg << g.inputs[i].c_str();
                tableMsg << (i < g.inputFlags.size() ? g.inputFlags[i].c_str() : "");
            }
        }
        tableMsg << osc::EndMessage;
    } catch (osc::OutOfBufferMemoryException&) {
        ss_log("device-table dropped: exceeds %d bytes", (int) sizeof(tableBuf));
        tableOk = false;
    }

    // Build output device list message
    // Format: mode(str), current(str), device1(str), ..., deviceN(str),
    //         sampleRate(int32), compat1(int32), ..., compatN(int32),
    //         type1(str), ..., typeN(str)
    //   compat: 1 = device supports current rate, 0 = rate change needed
    //   type:   driver type per device, enables the GUI's per-driver
    //           dropdown filter
    char devBuf[8192];
    osc::OutboundPacketStream devMsg(devBuf, sizeof(devBuf));
    devMsg << osc::BeginMessage("/supersonic/devices")
           << (mode.empty() ? "system" : mode.c_str())
           << current.name.c_str();
    for (auto& dev : outputDevices)
        devMsg << dev.name.c_str();
    devMsg << static_cast<osc::int32>(current.activeSampleRate);
    int curRate = static_cast<int>(current.activeSampleRate);
    for (auto& dev : outputDevices) {
        bool compat = false;
        for (auto r : dev.availableSampleRates)
            if (static_cast<int>(r) == curRate)
                compat = true;
        devMsg << static_cast<osc::int32>(compat ? 1 : 0);
    }
    for (auto& dev : outputDevices)
        devMsg << dev.typeName.c_str();
    devMsg << osc::EndMessage;

    // Build input device list message
    // Format: currentInput(str), numDevices(int32),
    //         name1(str), ..., nameN(str),
    //         type1(str), ..., typeN(str)
    char inDevBuf[2048];
    osc::OutboundPacketStream inDevMsg(inDevBuf, sizeof(inDevBuf));
    inDevMsg << osc::BeginMessage("/supersonic/input-devices")
             << current.inputDeviceName.c_str()
             << static_cast<osc::int32>(inputDevices.size());
    for (auto& dev : inputDevices)
        inDevMsg << dev.name.c_str();
    for (auto& dev : inputDevices)
        inDevMsg << dev.typeName.c_str();
    inDevMsg << osc::EndMessage;

    // Build hardware info message
    double sr = current.activeSampleRate;
    double outLatMs = sr > 0 ? (current.outputLatencySamples / sr) * 1000.0 : 0.0;
    double inLatMs  = sr > 0 ? (current.inputLatencySamples  / sr) * 1000.0 : 0.0;

    // Use per-device channel counts (maxOutputChannels / maxInputChannels)
    // rather than the aggregate's activeOutputChannels / activeInputChannels.
    // When running on an aggregate, active counts are sums across sub-devices
    // (MBP Speakers 2 out + MOTU 8 out = 10) which is confusing when the
    // user picked MBP Speakers as the output — they see "10 out" on a
    // 2-channel device. currentDevice() populates maxOutputChannels /
    // maxInputChannels with the real underlying sub-device counts.
    int outCh = current.maxOutputChannels > 0 ? current.maxOutputChannels
                                              : current.activeOutputChannels;
    // Only report an input count when input channels are actually open —
    // maxInputChannels is the device's capability and is non-zero even
    // when inputs are disabled (the patchbay always exposes 16 port
    // names), which would show "in 16" on a device with no input active.
    int inCh  = current.activeInputChannels == 0 ? 0
              : current.maxInputChannels  > 0    ? current.maxInputChannels
                                                 : current.activeInputChannels;

    char info[1024];
    if (!current.inputDeviceName.empty() && inCh > 0) {
        snprintf(info, sizeof(info),
                 "Output:      %s (%d ch)\n"
                 "Input:       %s (%d ch)\n"
                 "Driver:      %s\n"
                 "Sample Rate: %.0f Hz\n"
                 "Buffer Size: %d samples\n"
                 "Latency:     %.1f / %.1f ms (out/in)",
                 current.name.c_str(), outCh,
                 current.inputDeviceName.c_str(), inCh,
                 current.typeName.c_str(),
                 sr,
                 current.activeBufferSize,
                 outLatMs, inLatMs);
    } else {
        snprintf(info, sizeof(info),
                 "Output:      %s (%d ch)\n"
                 "Driver:      %s\n"
                 "Sample Rate: %.0f Hz\n"
                 "Buffer Size: %d samples\n"
                 "Latency:     %.1f ms",
                 current.name.c_str(), outCh,
                 current.typeName.c_str(),
                 sr,
                 current.activeBufferSize,
                 outLatMs);
    }

    // Info message with config data appended
    // Format: info_string, sampleRate(int32), bufferSize(int32),
    //         numRates(int32), rate1..rateN, numBufs(int32), buf1..bufN,
    //         numDrivers(int32), driver1..driverN, currentDriver(str),
    //         outputChannels(int32), inputChannels(int32),
    //         outputLatencySamples(int32),
    //         intendedDriver(str — pending switchDriver pick, "" = none)
    // Trailing fields are positional and optional: parsers pop in this
    // exact order, so new fields append at the END only.
    auto drivers = listDrivers();
    auto curDriver = currentDriver();

    // Compute usable sample rates: intersection of output and input device rates.
    // If no input device is active, use the output device's rates.
    //
    // When an aggregate is active, constrain to the CURRENT rate only.
    // Rate changes on a live aggregate are not reliable: pre-aligning
    // sub-devices in AggregateDeviceHelper::createOrUpdate fails because
    // the old aggregate still owns the sub-devices (destroying it early
    // crashes JUCE's AudioComponentInstanceDispose on the dangling id),
    // so the new aggregate inherits the old sub-device rates and the
    // requested rate gets snapped back by CoreAudio within a few audio
    // callbacks. Rather than expose rates that silently fail, show only
    // the current rate — the user can change rate by first switching to
    // a non-aggregated device (so the aggregate is torn down), then
    // changing rate, then re-adding the mic.
    std::vector<double> usableRates = current.availableSampleRates;
    bool onAggregate = !realOutputDeviceName().empty();
    if (onAggregate) {
        // Offer the rates BOTH sub-devices natively support — not just the
        // current one — so the rate is selectable on macOS. A rate only one
        // side supports would force aggregate-internal SRC, so it's excluded.
        std::vector<int> outRates, inRates;
        const std::string ro = realOutputDeviceName();
        const std::string ri = realInputDeviceName();
        for (auto& dev : allDevices) {
            if (dev.name == ro)
                for (auto r : dev.availableSampleRates)
                    outRates.push_back(static_cast<int>(r));
            if (!ri.empty() && dev.name == ri)
                for (auto r : dev.availableSampleRates)
                    inRates.push_back(static_cast<int>(r));
        }
        auto offered = sonicpi::device::usableAggregateRates(outRates, inRates);
        if (!offered.empty())
            usableRates.assign(offered.begin(), offered.end());
    } else if (!current.inputDeviceName.empty()) {
        for (auto& dev : allDevices) {
            if (dev.name == current.inputDeviceName) {
                std::vector<double> intersection;
                for (auto r : current.availableSampleRates)
                    for (auto ir : dev.availableSampleRates)
                        if (static_cast<int>(r) == static_cast<int>(ir))
                            intersection.push_back(r);
                if (!intersection.empty())
                    usableRates = intersection;
                break;
            }
        }
    }

    char infoBuf[4096];
    osc::OutboundPacketStream infoMsg(infoBuf, sizeof(infoBuf));
    infoMsg << osc::BeginMessage("/supersonic/info")
            << info
            << static_cast<osc::int32>(current.activeSampleRate)
            << static_cast<osc::int32>(current.activeBufferSize)
            << static_cast<osc::int32>(usableRates.size());
    for (auto r : usableRates)
        infoMsg << static_cast<osc::int32>(r);

    // Same intersection for buffer sizes (skip when on aggregate)
    std::vector<int> usableBufferSizes = current.availableBufferSizes;
    if (!onAggregate && !current.inputDeviceName.empty()) {
        for (auto& dev : allDevices) {
            if (dev.name == current.inputDeviceName) {
                std::vector<int> intersection;
                for (auto b : current.availableBufferSizes)
                    for (auto ib : dev.availableBufferSizes)
                        if (b == ib)
                            intersection.push_back(b);
                if (!intersection.empty())
                    usableBufferSizes = intersection;
                break;
            }
        }
    }

    // Filter to a canonical set of useful buffer sizes. Raw CoreAudio
    // lists up to 10+ sizes including weird non-powers-of-two (14, 24,
    // 48, 96) and extreme values (16, 8192). Most Sonic Pi users only
    // want 64–2048. On an aggregate with kernel drift correction,
    // buffers below 256 starve the drift compensator — the IOProc's
    // sample-rate conversion can't keep up and the audio warbles (the
    // "drift storm" the user reported after picking bs=16 on an
    // aggregate). Raise the minimum in that case.
    //
    // This is a DISPLAY filter, not a safety net: -Z / -z CLI flags and
    // TOML sound_card_buffer_size can still force any value for power
    // users who know what they're doing.
    {
        // Non-aggregate and same-clock aggregate: include small sizes
        // for low-latency use. Only drift-compensated aggregates force
        // the 256-sample floor — SRC IOProc starves at tight buffers
        // and produces warbling. Same-clock aggregates (e.g. MBP
        // Speakers + MBP Mic, both Apple Silicon built-in) have no SRC
        // running and can handle 16/32 just like a single device.
        static const int kCanonicalSingle[] = {16, 32, 64, 128, 256, 512, 1024, 2048};
        static const int kCanonicalAggregate[] = {256, 512, 1024, 2048};
        std::set<int> canonical;
#ifdef __APPLE__
        bool driftAggregate = onAggregate
            && AggregateDeviceHelper::driftCompensationEnabled();
#else
        bool driftAggregate = false;
#endif
        if (driftAggregate) {
            for (int b : kCanonicalAggregate) canonical.insert(b);
        } else {
            for (int b : kCanonicalSingle) canonical.insert(b);
        }
        std::vector<int> filtered;
        for (int b : usableBufferSizes)
            if (canonical.count(b)) filtered.push_back(b);
        if (!filtered.empty()) usableBufferSizes = std::move(filtered);
        // Also ensure the currently-active buffer size is represented so
        // the GUI dropdown can display the correct selection when a
        // non-canonical size was forced via CLI / TOML.
        if (current.activeBufferSize > 0) {
            bool present = false;
            for (int b : usableBufferSizes)
                if (b == current.activeBufferSize) { present = true; break; }
            if (!present) {
                usableBufferSizes.insert(usableBufferSizes.begin(), current.activeBufferSize);
            }
        }
    }

    infoMsg << static_cast<osc::int32>(usableBufferSizes.size());
    for (auto b : usableBufferSizes)
        infoMsg << static_cast<osc::int32>(b);
    infoMsg << static_cast<osc::int32>(drivers.size());
    for (auto& d : drivers)
        infoMsg << d.c_str();
    infoMsg << curDriver.c_str();
    // Send per-device counts (outCh/inCh), not aggregate sums — same
    // semantics as the banner. GUI shows these as "out N | in M" in the
    // SuperSonic summary label.
    infoMsg << static_cast<osc::int32>(outCh);
    infoMsg << static_cast<osc::int32>(inCh);
    // Device output latency: DSP-computed audio reaches the speaker this
    // many samples later.
    infoMsg << static_cast<osc::int32>(current.outputLatencySamples);
    // Pending driver pick (switchDriver with no openable device yet).
    // curDriver stays truthful about the audio path; this lets the GUI
    // keep its driver dropdown on the user's uncommitted choice — and,
    // when empty, tells it no pick is pending so a stale local override
    // must follow curDriver instead of pinning forever.
    infoMsg << mIntendedDriver.c_str();
    infoMsg << osc::EndMessage;

    // Fan out to all registered notify subscribers via the transport. The
    // table goes first so a client already holds the grouped lists when the
    // flat messages trigger its UI rebuild (relay order is not guaranteed;
    // clients must still tolerate either order).
    if (tableOk)
        mEgress.broadcastToTargets(reinterpret_cast<const uint8_t*>(tableMsg.Data()),
                                   static_cast<uint32_t>(tableMsg.Size()));
    mEgress.broadcastToTargets(reinterpret_cast<const uint8_t*>(devMsg.Data()),
                               static_cast<uint32_t>(devMsg.Size()));
    mEgress.broadcastToTargets(reinterpret_cast<const uint8_t*>(inDevMsg.Data()),
                               static_cast<uint32_t>(inDevMsg.Size()));
    mEgress.broadcastToTargets(reinterpret_cast<const uint8_t*>(infoMsg.Data()),
                               static_cast<uint32_t>(infoMsg.Size()));
}

bool SupersonicEngine::interceptBufferFreed(const uint8_t* data, uint32_t size) {
    // Quick prefix check — "/supersonic/buffer/freed" starts with '/'
    if (size < 28 || data[0] != '/') return false;
    try {
        osc::ReceivedPacket pkt(reinterpret_cast<const char*>(data),
                                static_cast<osc::osc_bundle_element_size_t>(size));
        osc::ReceivedMessage msg(pkt);
        if (std::strcmp(msg.AddressPattern(), "/supersonic/buffer/freed") != 0)
            return false;

        auto it = msg.ArgumentsBegin();
        int bufnum = 0;
        uintptr_t ptr = 0;
        if (it != msg.ArgumentsEnd()) { bufnum = it->AsInt32Unchecked(); ++it; }
        if (it != msg.ArgumentsEnd()) { ptr = static_cast<uintptr_t>(it->AsInt64Unchecked()); }

        if (ptr) zfree(reinterpret_cast<void*>(ptr));
        mStateCache.uncacheBuffer(bufnum);
        return true;
    } catch (...) {
        return false;
    }
}

void SupersonicEngine::interceptForCache(const uint8_t* data, uint32_t size) {
    try {
        osc::ReceivedPacket pkt(reinterpret_cast<const char*>(data),
                                static_cast<osc::osc_bundle_element_size_t>(size));
        osc::ReceivedMessage msg(pkt);
        const char* addr = msg.AddressPattern();

        if (std::strcmp(addr, "/d_recv") == 0) {
            // Extract synthdef blob, parse name, cache it
            auto it = msg.ArgumentsBegin();
            if (it != msg.ArgumentsEnd() && it->IsBlob()) {
                const void* blobData;
                osc::osc_bundle_element_size_t blobSize;
                it->AsBlob(blobData, blobSize);
                if (blobData && blobSize > 0) {
                    auto* blobBytes = static_cast<const uint8_t*>(blobData);
                    std::string name = StateCache::extractSynthDefName(blobBytes, blobSize);
                    if (!name.empty()) {
                        mStateCache.cacheSynthDef(name,
                            std::vector<uint8_t>(blobBytes, blobBytes + blobSize));
                    }
                }
            }
        } else if (std::strcmp(addr, "/d_free") == 0) {
            for (auto it = msg.ArgumentsBegin(); it != msg.ArgumentsEnd(); ++it) {
                if (it->IsString())
                    mStateCache.uncacheSynthDef(it->AsStringUnchecked());
            }
        } else if (std::strcmp(addr, "/d_freeAll") == 0) {
            mStateCache.clearSynthDefs();
        } else if (std::strcmp(addr, "/b_allocRead") == 0) {
            auto it = msg.ArgumentsBegin();
            int bufnum = 0, startFrame = 0, numFrames = 0;
            std::string path;
            if (it != msg.ArgumentsEnd()) { bufnum = it->AsInt32Unchecked(); ++it; }
            if (it != msg.ArgumentsEnd()) { path = it->AsStringUnchecked(); ++it; }
            if (it != msg.ArgumentsEnd()) { startFrame = it->AsInt32Unchecked(); ++it; }
            if (it != msg.ArgumentsEnd()) { numFrames = it->AsInt32Unchecked(); ++it; }
            mStateCache.cacheBuffer({bufnum, path, startFrame, numFrames, 0, 0});
        } else if (std::strcmp(addr, "/b_free") == 0) {
            auto it = msg.ArgumentsBegin();
            if (it != msg.ArgumentsEnd())
                mStateCache.uncacheBuffer(it->AsInt32Unchecked());
        }
    } catch (...) {
        // Don't let cache interception errors break message delivery
    }
}

// --- Variadic send helpers ---

void SupersonicEngine::sendBundle(double ntpTimeSec, std::initializer_list<OscPacket> messages) {
    const uint64_t tag =
        static_cast<uint64_t>(supersonic::ntpToOscTimetag(ntpTimeSec));
    auto pkt = OscBuilder::bundle(tag, messages);
    sendOSC(pkt.ptr(), pkt.size());
}

// --- Device management ---

std::vector<DeviceInfo> SupersonicEngine::listDevices(bool rescan) const {
    // Serialise mDeviceManager access against device mutations / recovery's
    // recreate (see mSwapMutex). Recursive: mutation paths call this under the
    // gate. Lock order is always gate-then-mListDevicesMutex.
    std::lock_guard<std::recursive_mutex> gate(mSwapMutex);
    std::vector<DeviceInfo> result;
    if (!mDeviceManager) return result;

    // Cache hit — see SupersonicEngine.h for the rationale (JUCE WASAPI
    // probing is ~10 s for a typical device set, called multiple times
    // during boot). Skip the cache when rescan=true (explicit refresh) or
    // when the cache has been invalidated by an audioDeviceListChanged.
    if (!rescan) {
        std::lock_guard<std::mutex> lk(mListDevicesMutex);
        if (!mCachedDevices.empty()
            && mCachedDevicesAt.time_since_epoch().count() != 0)
            return mCachedDevices;
    }

#ifdef __APPLE__
    // Build a name→transportType map from CoreAudio for all devices
    std::map<std::string, uint32_t> transportMap;
    {
        AudioObjectPropertyAddress pa = {
            kAudioHardwarePropertyDevices,
            kAudioObjectPropertyScopeGlobal,
            kAudioObjectPropertyElementMain
        };
        UInt32 dataSize = 0;
        if (AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &pa, 0, nullptr, &dataSize) == noErr) {
            auto count = dataSize / sizeof(AudioObjectID);
            std::vector<AudioObjectID> ids(count);
            if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &pa, 0, nullptr, &dataSize, ids.data()) == noErr) {
                for (auto id : ids) {
                    // Get name
                    AudioObjectPropertyAddress nameAddr = {
                        kAudioDevicePropertyDeviceNameCFString,
                        kAudioObjectPropertyScopeGlobal,
                        kAudioObjectPropertyElementMain
                    };
                    CFStringRef cfName = nullptr;
                    UInt32 nameSize = sizeof(cfName);
                    if (AudioObjectGetPropertyData(id, &nameAddr, 0, nullptr, &nameSize, &cfName) != noErr)
                        continue;
                    char buf[256];
                    CFStringGetCString(cfName, buf, sizeof(buf), kCFStringEncodingUTF8);
                    CFRelease(cfName);

                    // Get transport type
                    AudioObjectPropertyAddress tAddr = {
                        kAudioDevicePropertyTransportType,
                        kAudioObjectPropertyScopeGlobal,
                        kAudioObjectPropertyElementMain
                    };
                    UInt32 transport = 0;
                    UInt32 tSize = sizeof(transport);
                    AudioObjectGetPropertyData(id, &tAddr, 0, nullptr, &tSize, &transport);

                    transportMap[std::string(buf)] = transport;
                }
            }
        }
    }
#endif

    // JUCE appends " (N)" suffixes to disambiguate duplicate CoreAudio names.
    // This lambda strips the suffix for fallback matching against CoreAudio names.
#ifdef __APPLE__
    // Build a parallel name→AudioObjectID map (alongside transportMap) so
    // we can query per-device CoreAudio properties without opening JUCE
    // devices — critical when an aggregate is active and probing a sub-
    // device via JUCE would disrupt the live callback.
    std::map<std::string, AudioObjectID> idMap;
    {
        AudioObjectPropertyAddress pa = {
            kAudioHardwarePropertyDevices,
            kAudioObjectPropertyScopeGlobal,
            kAudioObjectPropertyElementMain
        };
        UInt32 dataSize = 0;
        if (AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &pa, 0, nullptr, &dataSize) == noErr) {
            auto count = dataSize / sizeof(AudioObjectID);
            std::vector<AudioObjectID> ids(count);
            if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &pa, 0, nullptr, &dataSize, ids.data()) == noErr) {
                for (auto id : ids) {
                    AudioObjectPropertyAddress nameAddr = {
                        kAudioDevicePropertyDeviceNameCFString,
                        kAudioObjectPropertyScopeGlobal,
                        kAudioObjectPropertyElementMain
                    };
                    CFStringRef cfName = nullptr;
                    UInt32 sz = sizeof(cfName);
                    if (AudioObjectGetPropertyData(id, &nameAddr, 0, nullptr, &sz, &cfName) != noErr || !cfName)
                        continue;
                    char buf[256];
                    CFStringGetCString(cfName, buf, sizeof(buf), kCFStringEncodingUTF8);
                    CFRelease(cfName);
                    idMap[std::string(buf)] = id;
                }
            }
        }
    }

    auto lookupTransport = [&transportMap](const std::string& juceName) -> uint32_t {
        auto it = transportMap.find(juceName);
        if (it != transportMap.end()) return it->second;
        for (auto& [caName, transport] : transportMap)
            if (sameDeviceName(juceName, caName)) return transport;
        return 0;
    };

    auto lookupID = [&idMap](const std::string& juceName) -> AudioObjectID {
        auto it = idMap.find(juceName);
        if (it != idMap.end()) return it->second;
        for (auto& [caName, id] : idMap)
            if (sameDeviceName(juceName, caName)) return id;
        return kAudioObjectUnknown;
    };

    // Channel count via CoreAudio's kAudioDevicePropertyStreamConfiguration.
    // Sums channels across all streams in the requested scope. Works
    // without opening the device via JUCE — so it's safe to call when
    // the aggregate is active and skipAllProbing is true.
    auto scopeChannelCount = [](AudioObjectID devID, bool isInput) -> int {
        if (devID == kAudioObjectUnknown) return 0;
        AudioObjectPropertyAddress addr = {
            kAudioDevicePropertyStreamConfiguration,
            isInput ? kAudioObjectPropertyScopeInput : kAudioObjectPropertyScopeOutput,
            kAudioObjectPropertyElementMain
        };
        UInt32 sz = 0;
        if (AudioObjectGetPropertyDataSize(devID, &addr, 0, nullptr, &sz) != noErr || sz == 0)
            return 0;
        std::vector<uint8_t> buf(sz);
        auto* bl = reinterpret_cast<AudioBufferList*>(buf.data());
        if (AudioObjectGetPropertyData(devID, &addr, 0, nullptr, &sz, bl) != noErr)
            return 0;
        int total = 0;
        for (UInt32 i = 0; i < bl->mNumberBuffers; ++i)
            total += (int)bl->mBuffers[i].mNumberChannels;
        return total;
    };
#endif

    // When an aggregate is active, DON'T probe any devices via createDevice.
    // Creating a JUCE AudioIODevice wrapper on a subdevice of our aggregate
    // (e.g. MacBook Pro Microphone when the aggregate owns it) and then
    // destroying it at end of unique_ptr scope closes the HAL IOProc and
    // silences the aggregate. We return device names without sample-rate /
    // buffer-size info in this case; cached values from pre-aggregate
    // enumeration remain with the GUI until the aggregate is torn down.
    bool skipAllProbing = false;
#ifdef __APPLE__
    if (AggregateDeviceHelper::exists())
        skipAllProbing = true;
#endif
    // Also skip probing the currently open device — its handle is live.
    std::string activeDeviceName;
    if (auto* dev = mDeviceManager->getCurrentAudioDevice())
        activeDeviceName = dev->getName().toStdString();

    auto& types = mDeviceManager->getAvailableDeviceTypes();
    for (auto* type : types) {
        if (rescan) type->scanForDevices();

        auto populateFromDevice = [](DeviceInfo& info, juce::AudioIODevice* dev) {
            for (auto r : dev->getAvailableSampleRates())
                info.availableSampleRates.push_back(r);
            for (auto b : dev->getAvailableBufferSizes())
                info.availableBufferSizes.push_back(b);
            info.maxOutputChannels = dev->getOutputChannelNames().size();
            info.maxInputChannels  = dev->getInputChannelNames().size();
            // Only the side the device was created on is authoritative: a
            // wrapper created output-only reports no input channels even on
            // a full-duplex device (the input merge below probes that side).
            info.outChannelsProbed = info.maxOutputChannels > 0;
            info.inChannelsProbed  = info.maxInputChannels > 0;
        };

        std::string typeNameStr = type->getTypeName().toStdString();
        auto shouldSkipProbe = [&](const std::string& name) {
            return skipAllProbing || name == activeDeviceName;
        };

        // ASIO names come from the registry, so an installed-but-unloadable
        // driver is listed exactly like a working one. Drop the ones the
        // Windows loader rejects outright — probing them yields no channels,
        // and the 0 -> 2/1 fallback below would dress them up as usable.
        const std::set<std::string> unloadable =
            typeNameStr == "ASIO" ? sonicpi::device::unloadableAsioDrivers()
                                  : std::set<std::string>{};

        // Enumerate output devices
        auto outputNames = type->getDeviceNames(false);
        for (auto& devName : outputNames) {
            DeviceInfo info;
            info.name = devName.toStdString();
            if (unloadable.count(info.name)) continue;
            info.typeName = typeNameStr;
#ifdef __APPLE__
            info.transportType = lookupTransport(info.name);
#endif

            if (!shouldSkipProbe(info.name)) {
                std::unique_ptr<juce::AudioIODevice> tempDev(
                    type->createDevice(devName, juce::String()));
                if (tempDev)
                    populateFromDevice(info, tempDev.get());
            }
#ifdef __APPLE__
            // Fill maxOutputChannels from CoreAudio if we don't already have
            // it (either because we skipped probing, or the JUCE probe
            // didn't return the full count). CoreAudio's stream
            // configuration is authoritative for a full-duplex multi-
            // channel device like a MOTU: no need to open the device,
            // so it's safe even while the aggregate is live.
            if (info.maxOutputChannels == 0) {
                AudioObjectID devID = lookupID(info.name);
                info.maxOutputChannels = scopeChannelCount(devID, false);
                info.outChannelsProbed = info.maxOutputChannels > 0;
            }
#else
            if (info.maxOutputChannels == 0) info.maxOutputChannels = 2;
#endif

            result.push_back(std::move(info));
        }

        // Enumerate input devices. A full-duplex device (e.g. a MOTU
        // soundcard with both playback and capture) shows up in both the
        // output and input enumerations with the same name. Merge the
        // input-side info into the existing entry so full-duplex inputs are
        // recorded and the GUI's input dropdown shows them.
        auto inputNames = type->getDeviceNames(true);
        for (auto& devName : inputNames) {
            std::string nameStr = devName.toStdString();
            if (unloadable.count(nameStr)) continue;

            DeviceInfo* existing = nullptr;
            for (auto& e : result) {
                if (e.name == nameStr && e.typeName == typeNameStr) {
                    existing = &e;
                    break;
                }
            }

            if (existing) {
                // Same device seen on the output side already. Add its
                // input capability to the existing entry.
                if (!shouldSkipProbe(existing->name)) {
                    std::unique_ptr<juce::AudioIODevice> tempDev(
                        type->createDevice(juce::String(), devName));
                    if (tempDev) {
                        existing->maxInputChannels =
                            tempDev->getInputChannelNames().size();
                        existing->inChannelsProbed =
                            existing->maxInputChannels > 0;
                    }
                }
#ifdef __APPLE__
                // Always check CoreAudio for the actual input count,
                // so full-duplex devices (MOTU etc.) report the true
                // number even when probing was skipped.
                if (existing->maxInputChannels == 0) {
                    AudioObjectID devID = lookupID(existing->name);
                    existing->maxInputChannels = scopeChannelCount(devID, true);
                    existing->inChannelsProbed = existing->maxInputChannels > 0;
                }
#else
                if (existing->maxInputChannels == 0)
                    existing->maxInputChannels = 1;
#endif
                continue;
            }

            // Input-only device (e.g. MacBook Pro Microphone).
            DeviceInfo info;
            info.name = std::move(nameStr);
            info.typeName = typeNameStr;
#ifdef __APPLE__
            info.transportType = lookupTransport(info.name);
#endif

            if (!shouldSkipProbe(info.name)) {
                std::unique_ptr<juce::AudioIODevice> tempDev(
                    type->createDevice(juce::String(), devName));
                if (tempDev)
                    populateFromDevice(info, tempDev.get());
            }
#ifdef __APPLE__
            if (info.maxInputChannels == 0) {
                AudioObjectID devID = lookupID(info.name);
                info.maxInputChannels = scopeChannelCount(devID, true);
                info.inChannelsProbed = info.maxInputChannels > 0;
            }
#else
            if (info.maxInputChannels == 0) info.maxInputChannels = 1;
#endif

            result.push_back(std::move(info));
        }
    }

#ifdef __APPLE__
    // Filter out our managed aggregate device — it's an implementation detail.
    if (AggregateDeviceHelper::exists()) {
        auto aggName = AggregateDeviceHelper::currentName();
        result.erase(
            std::remove_if(result.begin(), result.end(),
                [&aggName](const DeviceInfo& d) { return d.name == aggName; }),
            result.end());
    }
#endif

    {
        std::lock_guard<std::mutex> lk(mListDevicesMutex);
        mCachedDevices = result;
        mCachedDevicesAt = std::chrono::steady_clock::now();
    }
    return result;
}

bool SupersonicEngine::isSyntheticDefaultPick(const std::string& name) const {
    if (name != sonicpi::device::kSystemDefaultTableName)
        return false;
    // A driver with a real device of this name (PipeWire) makes it a
    // literal pick; everywhere else the row exists only in the table.
    const std::string driver = currentDriver();
    for (const auto& dev : listDevices(false))
        if (dev.typeName == driver && dev.name == name)
            return false;
    return true;
}

CurrentDeviceInfo SupersonicEngine::currentDevice() const {
    // Serialise mDeviceManager access against device mutations / recovery's
    // recreate (see mSwapMutex). Recursive: mutation paths call this under gate.
    std::lock_guard<std::recursive_mutex> gate(mSwapMutex);
    CurrentDeviceInfo info;
    if (!mDeviceManager) return info;

    auto* dev = mDeviceManager->getCurrentAudioDevice();
    if (!dev) return info;

    info.name     = dev->getName().toStdString();
    info.typeName = dev->getTypeName().toStdString();
    info.activeSampleRate    = dev->getCurrentSampleRate();
    info.activeBufferSize    = dev->getCurrentBufferSizeSamples();
    info.controlBlockSize    = mAudioCallback.bufferLength();
    info.activeOutputChannels = dev->getActiveOutputChannels().countNumberOfSetBits();
    info.activeInputChannels  = dev->getActiveInputChannels().countNumberOfSetBits();
    info.outputLatencySamples = dev->getOutputLatencyInSamples();
    info.inputLatencySamples  = dev->getInputLatencyInSamples();

    juce::AudioDeviceManager::AudioDeviceSetup setup;
    mDeviceManager->getAudioDeviceSetup(setup);
    info.inputDeviceName = setup.inputDeviceName.toStdString();

    // ASIO drivers are full-duplex single-device by spec — one
    // AudioIODevice carries both directions. JUCE has no separate
    // input-device concept on ASIO; setup.inputDeviceName echoes
    // back whatever the caller passed in, regardless of what's
    // actually open. Derive the truthful input name from the
    // output device when on ASIO and the input is active.
    if (info.typeName == "ASIO")
        info.inputDeviceName = (info.activeInputChannels > 0) ? info.name : "";

    // With zero active input channels there is no usable input, whatever
    // name the setup echoes back (JUCE fills in the driver's default-input
    // name even when the device was opened output-only). Report none so
    // the GUI's input dropdown matches reality.
    if (info.activeInputChannels == 0)
        info.inputDeviceName.clear();

    // If running on an aggregate device, report the real underlying names
    // so the GUI sees the actual hardware, not "SuperSonic".
    if (!mRealOutputDeviceName.empty())
        info.name = mRealOutputDeviceName;
    if (!mRealInputDeviceName.empty())
        info.inputDeviceName = mRealInputDeviceName;

    for (auto r : dev->getAvailableSampleRates())
        info.availableSampleRates.push_back(r);
    for (auto b : dev->getAvailableBufferSizes())
        info.availableBufferSizes.push_back(b);

    // Populate max channel counts. When on aggregate, query the real
    // underlying devices (not the aggregate wrapper — which reports
    // the union of sub-device channels). When on a plain device,
    // JUCE's channel-name lists are accurate.
    info.maxOutputChannels = dev->getOutputChannelNames().size();
    info.maxInputChannels  = dev->getInputChannelNames().size();
#ifdef __APPLE__
    auto caChannelCount = [](const std::string& name, bool isInput) -> int {
        if (name.empty()) return 0;
        AudioObjectPropertyAddress listAddr = {
            kAudioHardwarePropertyDevices,
            kAudioObjectPropertyScopeGlobal,
            kAudioObjectPropertyElementMain
        };
        UInt32 listSize = 0;
        if (AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &listAddr, 0, nullptr, &listSize) != noErr)
            return 0;
        std::vector<AudioObjectID> ids(listSize / sizeof(AudioObjectID));
        if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &listAddr, 0, nullptr, &listSize, ids.data()) != noErr)
            return 0;
        for (auto id : ids) {
            AudioObjectPropertyAddress nameAddr = {
                kAudioDevicePropertyDeviceNameCFString,
                kAudioObjectPropertyScopeGlobal,
                kAudioObjectPropertyElementMain
            };
            CFStringRef cfName = nullptr;
            UInt32 sz = sizeof(cfName);
            if (AudioObjectGetPropertyData(id, &nameAddr, 0, nullptr, &sz, &cfName) != noErr || !cfName)
                continue;
            char buf[256];
            CFStringGetCString(cfName, buf, sizeof(buf), kCFStringEncodingUTF8);
            CFRelease(cfName);
            if (name != buf) continue;
            AudioObjectPropertyAddress scopeAddr = {
                kAudioDevicePropertyStreamConfiguration,
                isInput ? kAudioObjectPropertyScopeInput : kAudioObjectPropertyScopeOutput,
                kAudioObjectPropertyElementMain
            };
            UInt32 cfgSize = 0;
            if (AudioObjectGetPropertyDataSize(id, &scopeAddr, 0, nullptr, &cfgSize) != noErr || cfgSize == 0)
                return 0;
            std::vector<uint8_t> cfgBuf(cfgSize);
            auto* bl = reinterpret_cast<AudioBufferList*>(cfgBuf.data());
            if (AudioObjectGetPropertyData(id, &scopeAddr, 0, nullptr, &cfgSize, bl) != noErr)
                return 0;
            int total = 0;
            for (UInt32 i = 0; i < bl->mNumberBuffers; ++i)
                total += (int)bl->mBuffers[i].mNumberChannels;
            return total;
        }
        return 0;
    };
    // Prefer real-device counts when we're on an aggregate.
    if (!mRealOutputDeviceName.empty()) {
        int n = caChannelCount(mRealOutputDeviceName, false);
        if (n > 0) info.maxOutputChannels = n;
    }
    if (!mRealInputDeviceName.empty()) {
        int n = caChannelCount(mRealInputDeviceName, true);
        if (n > 0) info.maxInputChannels = n;
    }
#endif

    return info;
}

// ── Audio source state machine ──────────────────────────────────────────────
//
// See the contract on the enum/helpers in SupersonicEngine.h.

SupersonicEngine::AudioSource SupersonicEngine::desiredAudioSource() const {
    if (mDeviceManager && mDeviceManager->getCurrentAudioDevice())
        return AudioSource::RealCallback;
    // Headless is an EXPLICIT mode only — the test harness and future non-JUCE /
    // WASM backends. For the default engine, no open device means None: a waiting
    // state the watchdog recovers from when a device appears.
    if (mHeadless)
        return AudioSource::Headless;
    return AudioSource::None;
}

void SupersonicEngine::startAudioSource() {
    if (mActiveSource.load() != AudioSource::None) {
        // Should be unreachable: every caller stops before starting.
        // Assert in debug so a regression fails CI loudly; log+return in
        // release so we don't crash a user session.
        jassertfalse;
        fprintf(stderr,
                "[supersonic] BUG: startAudioSource called while %s already active\n",
                mActiveSource.load() == AudioSource::RealCallback ? "RealCallback" : "Headless");
        fflush(stderr);
        return;
    }

    // Manual-pump mode (tests): start no audio source at all. The caller
    // owns process_audio() on its own thread; starting an autonomous audio
    // thread (real callback or headless driver) here would give two
    // concurrent callers of process_audio() — a data race on the whole
    // engine world. Leave mActiveSource == None so shutdown's
    // stopAudioSource() no-ops; mRunning is still set by init() afterwards.
    if (mCurrentConfig.manualAudioPump) {
        fprintf(stderr,
                "[supersonic] manual audio pump — no audio source started; "
                "caller drives process_audio()\n");
        fflush(stderr);
        mActiveSource.store(AudioSource::None, std::memory_order_release);
        return;
    }

    uint32_t before = mAudioCallback.processCount.load(std::memory_order_acquire);

    const AudioSource desired = desiredAudioSource();
    if (desired == AudioSource::RealCallback) {
        // Bracket the attach with always-on log lines: a silent process
        // death in this window (seen in the wild on a virtual 8-out device)
        // is only localisable if the log shows exactly how far we got —
        // nothing after "attaching" = died inside the driver's start path;
        // "attached" but no "[juce] first audio callback" = died before the
        // device's IO thread reached our callback.
        {
            auto* dev = mDeviceManager->getCurrentAudioDevice();
            fprintf(stderr, "[supersonic] attaching audio callback to device '%s'\n",
                    dev ? dev->getName().toRawUTF8() : "(none)");
            fflush(stderr);
        }
        mDeviceManager->addAudioCallback(&mAudioCallback);
        fprintf(stderr, "[supersonic] audio callback attached — waiting for first tick\n");
        fflush(stderr);
        // addChangeListener is idempotent (JUCE's ListenerList dedupes), so
        // re-attaching across hot-plug / swap sequences is harmless.
        mDeviceManager->addChangeListener(this);
        mActiveSource.store(AudioSource::RealCallback, std::memory_order_release);
    } else if (desired == AudioSource::Headless) {
        // Explicit headless (mHeadless): tests and future non-JUCE backends.
        mHeadlessDriver.configure(&mAudioCallback, &mSampleLoader,
                                   mCurrentConfig.sampleRate,
                                   mCurrentConfig.bufferSize,
                                   mCurrentConfig.numOutputChannels,
                                   mCurrentConfig.numInputChannels);
        mHeadlessDriver.setSuperClock(&mSuperClock);
        mHeadlessDriver.startThread(juce::Thread::Priority::highest);
        mActiveSource.store(AudioSource::Headless, std::memory_order_release);
    } else {
        // No audio device on the default (JUCE) engine: stay sourceless and
        // surface it. The watchdog keeps trying to open a device and cold-swaps
        // in when one appears (plug in / wake). audioSource()==None with a live
        // device manager is the "waiting for audio device" state.
        mActiveSource.store(AudioSource::None, std::memory_order_release);
        ss_log("[supersonic] no audio device available — engine is idle and will "
               "recover when one appears");
        sendDeviceReport();   // GUI sees an empty current device
        return;               // nothing will tick; don't wait for a first block
    }

    waitForFirstAudioTick(before);
}

void SupersonicEngine::stopAudioSource() {
    switch (mActiveSource.load()) {
    case AudioSource::None:
        return;
    case AudioSource::RealCallback:
        if (mDeviceManager)
            mDeviceManager->removeAudioCallback(&mAudioCallback);
        // Change listener is NOT removed here; it survives swaps and is
        // removed only in shutdown(). Removing it would lose hot-plug
        // events between stop and the next start.
        break;
    case AudioSource::Headless:
        mHeadlessDriver.signalThreadShouldExit();
        mHeadlessDriver.stopThread(2000);
        break;
    }
    mActiveSource.store(AudioSource::None, std::memory_order_release);
}

void SupersonicEngine::waitForFirstAudioTick(uint32_t before) {
    constexpr int kTimeoutMs = 5000;
    auto start = std::chrono::steady_clock::now();
    auto deadline = start + std::chrono::milliseconds(kTimeoutMs);
    while (mAudioCallback.processCount.load(std::memory_order_acquire) == before
           && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    bool ticked = mAudioCallback.processCount.load(std::memory_order_acquire) != before;
    if (!ticked) {
        fprintf(stderr,
                "[supersonic] WARNING: audio callbacks not firing after %d ms, "
                "engine is alive but the audio thread has not started\n", kTimeoutMs);
        fflush(stderr);
    } else {
        auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start).count();
        ssLifecycleLog("[supersonic] audio callbacks started (%lld ms)\n",
                       static_cast<long long>(elapsedMs));
    }
}

juce::String SupersonicEngine::reinitialiseWithDefaultsPreservingConfig() {
    int prevRate = mCurrentConfig.sampleRate;
    auto prevSetup = mDeviceManager->getAudioDeviceSetup();
    int prevBufSize = prevSetup.bufferSize;

#ifdef _WIN32
    // "System default" has no meaning under ASIO: JUCE's ASIO type has no
    // default device, so initialiseWithDefaultDevices from an ASIO session
    // stops the current device and opens none, leaving the engine with no
    // device and no callbacks. Return to the boot-time type (mBootDriver —
    // see init's type selection) before asking for defaults; when boot
    // itself was ASIO (-H onto an ASIO device), DirectSound stands in as
    // the type that can serve a default.
    {
        auto curType = mDeviceManager->getCurrentAudioDeviceType();
        if (curType == "ASIO") {
            const std::string target =
                (!mBootDriver.empty() && mBootDriver != "ASIO")
                    ? mBootDriver : std::string("DirectSound");
            fprintf(stderr, "[device-setup] system default requested from ASIO "
                    "— returning to %s first\n", target.c_str());
            fflush(stderr);
            mLastSelfTriggeredChange = std::chrono::steady_clock::now();
            mDeviceManager->setCurrentAudioDeviceType(
                juce::String(target), true);
        }
    }
#endif

    // Refresh suppression timestamp before each JUCE call to prevent
    // changeListenerCallback feedback storms.
    mLastSelfTriggeredChange = std::chrono::steady_clock::now();
    auto err = mDeviceManager->initialiseWithDefaultDevices(0, 2);
    if (err.isNotEmpty()) {
        mLastSelfTriggeredChange = std::chrono::steady_clock::now();
        err = mDeviceManager->initialiseWithDefaultDevices(0, 0);
    }
    if (err.isNotEmpty()) return err;

    // An init that reports success but opens no device is still a failure
    // here: callers assume a current device afterwards, and without one the
    // audio callback never restarts and the engine produces no sound. Report
    // it so the caller can recover.
    if (!mDeviceManager->getCurrentAudioDevice())
        return "system default init opened no device";

    // Don't second-guess JUCE's negotiation on wireless devices. AirPlay
    // / Bluetooth negotiate a specific buffer size with the remote end
    // (often 512+); JUCE reports many buffer sizes as "available" but in
    // practice only the negotiated one actually delivers audio. Forcing
    // a different size runs the IOProc (scope stays happy) but produces
    // silence at the speakers. For non-wireless devices we still
    // preserve the user's previous rate/buffer so GUI selections stick
    // across System Output toggles.
    auto setup = mDeviceManager->getAudioDeviceSetup();
    bool isWireless = false;
#ifdef __APPLE__
    if (auto* dev = mDeviceManager->getCurrentAudioDevice()) {
        std::string curName = dev->getName().toStdString();
        for (auto& d : listDevices(false)) {
            if (sameDeviceName(d.name, curName) && d.isWirelessTransport()) {
                isWireless = true;
                break;
            }
        }
    }
#endif
    if (!isWireless) {
        setup.sampleRate = static_cast<double>(prevRate);
        setup.bufferSize = prevBufSize;
        mLastSelfTriggeredChange = std::chrono::steady_clock::now();
        mDeviceManager->setAudioDeviceSetup(setup, true);
    } else {
        // Wireless device is now active. AirPlay 1 negotiates 44.1 kHz
        // with the receiver, but AirPlay 2 receivers commonly support
        // 48 kHz. Probe the device's available rates — if prevRate is
        // in the list, force it rather than silently accepting the
        // receiver's default. That way a modern AirPlay 2 speaker
        // doesn't downgrade a 48 kHz session to 44.1.
        bool forcedPrev = false;
        if (auto* dev = mDeviceManager->getCurrentAudioDevice()) {
            bool prevSupported = false;
            for (auto r : dev->getAvailableSampleRates()) {
                if (static_cast<int>(r) == prevRate) {
                    prevSupported = true;
                    break;
                }
            }
            if (prevSupported && static_cast<int>(setup.sampleRate) != prevRate) {
                setup.sampleRate = static_cast<double>(prevRate);
                mLastSelfTriggeredChange = std::chrono::steady_clock::now();
                auto err2 = mDeviceManager->setAudioDeviceSetup(setup, true);
                if (err2.isEmpty()) {
                    forcedPrev = true;
                    fprintf(stderr, "[device-setup] reinit: wireless device supports "
                            "prev rate %d — forcing it (was %.0f)\n",
                            prevRate, dev->getCurrentSampleRate());
                    fflush(stderr);
                } else {
                    fprintf(stderr, "[device-setup] reinit: setAudioDeviceSetup at "
                            "prev rate %d failed on wireless (%s), keeping negotiated\n",
                            prevRate, err2.toRawUTF8());
                    fflush(stderr);
                }
            }
        }
        if (!forcedPrev) {
            fprintf(stderr, "[device-setup] reinit: keeping JUCE's negotiated rate=%.0f buf=%d "
                    "for wireless device (prev rate=%d buf=%d)\n",
                    setup.sampleRate, setup.bufferSize, prevRate, prevBufSize);
            fflush(stderr);
        }
    }

    // Post-init cleanup: we just switched away from whatever device we
    // were on (aggregate or otherwise) to JUCE's default. Drop the stale
    // real-device-name state so currentDevice() and any following reopen target
    // the actual new device, not the previous pinned/aggregate names. This must
    // run on every platform — a later reopenCurrentDevice reads
    // mRealOutputDeviceName, so a stale name on Win/Linux would reopen a gone
    // device. The aggregate teardown itself is macOS-only.
    mRealOutputDeviceName.clear();
    mRealInputDeviceName.clear();
#ifdef __APPLE__
    AggregateDeviceHelper::destroyPrevious();
    if (AggregateDeviceHelper::exists()) {
        AggregateDeviceHelper::destroy();
    }
#endif
    return {};
}

SwapResult SupersonicEngine::switchDevice(const std::string& rawOutputName,
                                           double sampleRate,
                                           int bufferSize,
                                           bool forceCold,
                                           const std::string& rawInputName,
                                           SwapOrigin origin) {
    // Normalise raw CoreAudio names to JUCE's disambiguated form. Callers
    // that source names from CoreAudio APIs (setDeviceMode's default-
    // output resolution, platform property listeners) otherwise hand raw
    // names straight to setAudioDeviceSetup, which errors "No such device"
    // when CoreAudio has duplicate base names (two identical USB
    // interfaces, two AirPlay endpoints with the same base name).
    // Caller-supplied names from the GUI dropdown are already JUCE-form
    // and pass through unchanged. Sentinels like "__system__" / "__none__"
    // don't match anything and also pass through. Shadowing the function
    // parameters keeps the rest of this function free to use the names
    // without thinking about which layer they came from.
    std::string deviceName = rawOutputName;
    std::string inputDeviceName = rawInputName;
    if (mDeviceManager && (!deviceName.empty() || !inputDeviceName.empty())) {
        std::vector<std::string> visibleNames;
        for (auto& d : listDevices(false)) visibleNames.push_back(d.name);
        deviceName      = sonicpi::device::resolveJuceDeviceName(deviceName, visibleNames);
        inputDeviceName = sonicpi::device::resolveJuceDeviceName(inputDeviceName, visibleNames);
    }

#if defined(__linux__) && defined(SUPERSONIC_PIPEWIRE)
    // The patchbay's two sides live on one filter node, so a mixed
    // patchbay/stream pair can never open as requested — JUCE would hand
    // both sides to whichever device the type resolves, silently
    // overriding the side the user just changed. Resolve to the pair that
    // will really open before any destructive swap work (see
    // DevicePolicy::resolveExclusiveDuplexPair for the rules). Gated on a
    // named request: rate/buffer-only swaps carry no pairing intent, and
    // currentDevice() takes the swap gate — a concurrent rate-only swap
    // must reach the reject-if-busy check without blocking on it.
    if (!deviceName.empty() || !inputDeviceName.empty()) {
        auto cur = currentDevice();
        const auto resolved = sonicpi::device::resolveExclusiveDuplexPair(
            deviceName, inputDeviceName, cur.name, cur.inputDeviceName,
            pipeWirePatchbayDeviceName(), pipeWireDefaultDeviceName());
        if (resolved.output != deviceName || resolved.input != inputDeviceName) {
            fprintf(stderr,
                    "[device-setup] exclusive-pair resolve: out '%s' -> '%s', in '%s' -> '%s'\n",
                    deviceName.c_str(), resolved.output.c_str(),
                    inputDeviceName.c_str(), resolved.input.c_str());
            deviceName = resolved.output;
            inputDeviceName = resolved.input;
        }
    }
#endif

    SwapResult result;
    result.deviceName = deviceName;
    bool recovered = false;

    // No-op detection: destroying and recreating an identical device is
    // fragile — CoreAudio sometimes stops a recreated aggregate within a
    // callback or two, and on Linux the ALSA close+reopen races PipeWire's
    // client-side node setup and can SIGSEGV inside libspa-audioconvert
    // (sonic-pi#3550; the GUI's boot-time saved-prefs restore sends exactly
    // such a same-device switch). If the caller asked for exactly what we
    // already have, short-circuit. A requested input counts as satisfied
    // only when it names the input that is currently open — enabling a
    // closed input is a real change and must reopen.
    if (!deviceName.empty() && sampleRate <= 0 && bufferSize <= 0 && !forceCold) {
        std::string activeReal = mRealOutputDeviceName.empty()
            ? (mDeviceManager && mDeviceManager->getCurrentAudioDevice()
               ? mDeviceManager->getCurrentAudioDevice()->getName().toStdString() : "")
            : mRealOutputDeviceName;
        bool inputSatisfied = inputDeviceName.empty();
        if (!inputSatisfied) {
            auto cur = currentDevice();
            inputSatisfied = cur.activeInputChannels > 0
                          && !cur.inputDeviceName.empty()
                          && cur.inputDeviceName == inputDeviceName;
        }
        if (activeReal == deviceName && inputSatisfied) {
            result.success = true;
            result.type = SwapType::Hot;
            result.deviceName = deviceName;
            result.sampleRate = mCurrentConfig.sampleRate;
            result.bufferSize = mCurrentConfig.bufferSize;
            return result;
        }
    }

    // Reject "add mic while current output is wireless" upfront, BEFORE
    // any cold-swap work. The aggregate filter later in this function
    // would drop the mic anyway, but by that point we've already
    // triggered a cold swap that can race the audio thread's scope UGen
    // and crash in ScopeOut2_next.
    if (auto err = refuseWirelessMicAddition(deviceName, inputDeviceName);
        !err.empty()) {
        result.error = err;
        return result;
    }

    // Reject swap with an unknown output / input device name BEFORE any
    // destructive state mutation. Without this, switchDevice mutates
    // mCurrentConfig.numInputChannels, opts[], destroys the World, and
    // pauses the audio callback before discovering at setAudioDeviceSetup
    // time that the name doesn't resolve. The half-built state poisons
    // the next cold-swap reinit (mixer_group never confirms via /n_go).
    if (auto err = refuseUnknownDeviceName(deviceName, inputDeviceName);
        !err.empty()) {
        result.error = err;
        fprintf(stderr, "[switchDevice] refused: %s\n", err.c_str());
        fflush(stderr);
        return result;
    }

    // Acquire the swap gate (non-blocking) — tryAcquireSwapGate is the
    // single way any code takes it.
    std::unique_lock<std::recursive_mutex> guard;
    if (!tryAcquireSwapGate(guard, 1, 0)) {
        result.error = "swap already in progress";
        return result;
    }
    PhaseGuard phase(mDevicePhase, DevicePhase::Swapping);
    // Entry+exit stamps: during the span the gate makes the MM handlers
    // skip; the exit stamp arms the quiet window against our own change
    // notifications. Mid-function stamps are gone — they silently expired
    // inside >1 s swaps.
    SelfTriggerSpan selfTrigger(mLastSelfTriggeredChange);
    RunLoopSuppressGuard runLoopSuppress { mSuppressRunLoop };

    // ── Plan ────────────────────────────────────────────────────────────
    // All decisions live in DevicePolicy::planSwap; this function only
    // gathers the snapshot the planner asks for and then executes the
    // plan. Two passes: the first resolves names/scope so the probes
    // (rates, channel counts) can target the right devices — and can be
    // skipped entirely when the first pass already forced a cold swap or
    // decided a rate, keeping probe costs identical to the old inline
    // code.
    sonicpi::device::SwapSnapshot snap;
    snap.hasDeviceManager = mDeviceManager != nullptr;
    if (mDeviceManager) {
        if (auto* dev = mDeviceManager->getCurrentAudioDevice()) {
            snap.juceCurrentType   = dev->getTypeName().toStdString();
            snap.currentOutputName = dev->getName().toStdString();
            snap.currentRate       = dev->getCurrentSampleRate();
        } else {
            snap.juceCurrentType =
                mDeviceManager->getCurrentAudioDeviceType().toStdString();
        }
        for (auto& d : listDevices(false)) {
            snap.deviceTable.emplace_back(d.typeName, d.name);
            if (d.isWirelessTransport())
                snap.wirelessDeviceNames.push_back(d.name);
        }
    } else {
        snap.currentRate = static_cast<double>(mCurrentConfig.sampleRate);
    }
    snap.intendedDriver        = mIntendedDriver;
    snap.deviceMode            = mDeviceMode;
    snap.currentOutputChannels = mCurrentConfig.numOutputChannels;
    snap.currentInputChannels  = mCurrentConfig.numInputChannels;
    snap.bootInputChannels     = mBootInputChannels;
    snap.preWirelessRate       = mPreWirelessRate;
    if (auto it = mDeviceRateMemory.find(deviceName);
        it != mDeviceRateMemory.end())
        snap.rememberedRate = it->second;

    sonicpi::device::SwapPlanRequest planReq;
    planReq.outputName    = deviceName;
    planReq.inputName     = inputDeviceName;
    planReq.sampleRate    = sampleRate;
    planReq.bufferSize    = bufferSize;
    planReq.forceCold     = forceCold;
    planReq.userInitiated = origin == SwapOrigin::User;

    auto plan = sonicpi::device::planSwap(planReq, snap);
    if (plan.error.empty()) {
        // Fill in the probes the first pass showed we need, then re-plan.
        if (sampleRate <= 0 && !plan.restoredPreWirelessRate) {
            snap.outputDeviceRates = probeDeviceSampleRates(deviceName, false);
            snap.inputDeviceRates  = probeDeviceSampleRates(plan.inputName, true);
        }
        if (plan.enableInputWidth >= 0)
            snap.probedInputChannels = probeDeviceChannelCount(
                plan.inputName, true, probeDriverTypeName(plan.scope));
        else if (mDeviceManager && !forceCold) {
            const std::string probeType = probeDriverTypeName(plan.scope);
            snap.probedTargetOut =
                probeDeviceChannelCount(deviceName, false, probeType);
            snap.probedTargetIn =
                probeDeviceChannelCount(plan.inputName, true, probeType);
        }
        plan = sonicpi::device::planSwap(planReq, snap);
    }
    if (!plan.error.empty()) {
        result.error = plan.error;
        return result;
    }

    // Execute the plan's bookkeeping decisions + the logs the old inline
    // code emitted (provenance flags exist for exactly this).
    if (plan.abandonDriverIntent) {
        fprintf(stderr,
            "[device-setup] abandoning pending driver intent '%s' "
            "— picks resolve under '%s'\n",
            mIntendedDriver.c_str(), snap.juceCurrentType.c_str());
        fflush(stderr);
        mIntendedDriver.clear();
    }
    sonicpi::device::SwapScope scope = plan.scope;
    if (scope.crossDriver) {
        fprintf(stderr,
            "[device-setup] cross-driver: '%s' -> '%s' (device '%s')\n",
            snap.juceCurrentType.c_str(), scope.targetDriver.c_str(),
            scope.targetDevice.c_str());
        fflush(stderr);
    }
    if (plan.inputName != inputDeviceName) {
        fprintf(stderr,
            "[device-setup] ASIO full-duplex: mirroring '%s' to input\n",
            plan.inputName.c_str());
        fflush(stderr);
    }
    inputDeviceName = plan.inputName;
    if (plan.restoredPreWirelessRate) {
        fprintf(stderr, "[device-setup] restoring pre-wireless rate %d "
                "(current=%.0f)\n", mPreWirelessRate, snap.currentRate);
        fflush(stderr);
    }
    if (plan.rateAdjustedToNearest) {
        fprintf(stderr,
            "[device-setup] current rate %.0f not supported "
            "by the target device, will use %.0f (cold swap)\n",
            snap.currentRate, plan.sampleRate);
        fflush(stderr);
    }
    sampleRate = plan.sampleRate;

    if (plan.enableInputWidth >= 0) {
#ifdef __APPLE__
        // Log mic permission status for diagnostics — but don't refuse
        // enabling inputs. supersonic's TCC query may return notDetermined
        // when launched as a child of the GUI, while CoreAudio's actual
        // mic stream honours the GUI's grant via responsible-process
        // attribution. Trying anyway may work; if buffers come back zero,
        // we'll know TCC really is denying.
        std::string micStat = MicPermission::status();
        if (micStat != "authorized") {
            fprintf(stderr, "[device-setup] mic permission status=%s (proceeding anyway; "
                    "CoreAudio may still grant via GUI's responsible process)\n",
                    micStat.c_str());
            fflush(stderr);
        }
#endif
        if (plan.enableInputWidth != plan.enableInputRequested) {
            fprintf(stderr, "[device-setup] auto-enabling %d input channels for '%s' "
                    "(requested %d, device max %d)\n",
                    plan.enableInputWidth, inputDeviceName.c_str(),
                    plan.enableInputRequested, plan.enableInputProbed);
        } else {
            fprintf(stderr, "[device-setup] auto-enabling %d input channels for '%s'\n",
                    plan.enableInputWidth, inputDeviceName.c_str());
        }
        mCurrentConfig.numInputChannels = plan.enableInputWidth;
        uint32_t* opts = reinterpret_cast<uint32_t*>(sp_arena() + WORLD_OPTIONS_START);
        opts[sonicpi::WorldOpts::kNumInputBusChannels] =
            static_cast<uint32_t>(plan.enableInputWidth);
    }

    if (plan.coldForChannels) {
        fprintf(stderr, "[device-setup] channel-count change detected "
                "(probedOut=%d probedIn=%d currentOut=%d currentIn=%d) "
                "— forcing cold swap so World rebuilds at new bus count\n",
                snap.probedTargetOut, snap.probedTargetIn,
                snap.currentOutputChannels, snap.currentInputChannels);
        fflush(stderr);
    }

    bool inputWasDropped = false;
    const bool   isCold      = plan.isCold;
    const double currentRate = snap.currentRate;
    result.type = isCold ? SwapType::Cold : SwapType::Hot;

    if (isCold) setEngineState(EngineState::Restarting, "rate-change");
    if (onSwapEvent) onSwapEvent("swap:start", result);

    // --- Pause and optionally capture state ---
    if (isCold) {
        purge();
        mSampleLoader.pauseLoading();
    }
    mAudioCallback.pause();

    // Cold swaps tear down and re-init the shared-memory world (destroy_world
    // / rebuild_world → init_memory → ss_lanes_reset_drains) that the reader
    // threads' drains walk concurrently — park them until the world is back.
    // Scope guard so every exit path below resumes them; the happy paths
    // resume explicitly just before audio restarts.
    struct ReaderPark {
        std::vector<std::function<void()>> resumers;
        void park(RingReader& r)      { r.pause(); resumers.push_back([&r] { r.resume(); }); }
        void resumeNow() { for (auto& f : resumers) f(); resumers.clear(); }
        ~ReaderPark()    { resumeNow(); }
    } readerPark;
    if (isCold)
        readerPark.park(mNrtGateway);

    if (isCold) mStateCache.captureAll();

    // --- Stop audio ---
    stopAudioSource();

    if (isCold) destroy_world();

    // Snapshot the current device setup so we can restore it if the new
    // setAudioDeviceSetup call below fails. JUCE's AudioDeviceManager can
    // be left with no bound device after a failed setAudioDeviceSetup
    // (e.g. user-selected name not in the device list, exclusive-mode
    // contention, sample-rate-not-supported). Without restoring, the
    // rollback below re-attaches the audio callback to a manager with
    // no device — symptom: device-list shows currentIn=''/currentOut='',
    // GUI prefs read 0hz | 0buf | 0out | 0in, every scsynth reply times
    // out because the audio thread isn't ticking.
    juce::AudioDeviceManager::AudioDeviceSetup prevSetup;
    if (mDeviceManager) mDeviceManager->getAudioDeviceSetup(prevSetup);

    // --- Apply new device configuration ---
    std::string errStr;
    if (mDeviceManager) {
        // Cross-driver: move JUCE to the new AudioIODeviceType before
        // reading the setup. setCurrentAudioDeviceType internally calls
        // setAudioDeviceSetup with the new type's saved (often empty)
        // config; insertDefaultDeviceNames fills the empty field with
        // the alphabetical-first device of the type. The transient
        // open is discardable — outputDeviceName is overridden below
        // and setAudioDeviceSetup is re-run authoritatively. On
        // Windows ASIO an unplugged-but-registered driver can hang
        // here in IASIO::init().
        if (scope.crossDriver) {
            // A cross-driver change costs ~1.5 s, and essentially all of it
            // is JUCE: setCurrentAudioDeviceType closes the open device and
            // then does a hardcoded Thread::sleep(1500): "allow a moment for
            // OS devices to sort themselves out, to help avoid things like
            // DirectSound/ASIO clashes" (juce_AudioDeviceManager.cpp).
            //
            // Closing the device first would make JUCE take neither branch,
            // but leaves the manager in a state its destructor cannot
            // handle: the process faults on teardown (0xC000041D). The safe
            // lever is shortening the sleep in the vendored JUCE via a
            // FetchContent patch, which changes the duration without
            // reordering the device lifecycle.
            mDeviceManager->setCurrentAudioDeviceType(
                juce::String(scope.targetDriver), false);
        }

        juce::AudioDeviceManager::AudioDeviceSetup setup;
        mDeviceManager->getAudioDeviceSetup(setup);

        if (scope.crossDriver) {
            // Override the names left by the transient open with the
            // resolved values: outputDeviceName to scope.targetDevice,
            // inputDeviceName to the local inputDeviceName (already
            // mirrored by the ASIO full-duplex auto-pick when empty).
            // An empty inputDeviceName here would re-trigger
            // insertDefaultDeviceNames, which picks the alphabetical-
            // first input of the new type.
            setup.outputDeviceName = juce::String(scope.targetDevice);
            setup.inputDeviceName  =
                (inputDeviceName.empty() || inputDeviceName == "__none__")
                    ? juce::String()
                    : juce::String(inputDeviceName);
        }

#ifdef __APPLE__
        // If currently on an aggregate device, resolve back to real device names
        // so we don't recursively wrap aggregates.
        if (AggregateDeviceHelper::exists()) {
            if (setup.outputDeviceName.toStdString() == AggregateDeviceHelper::currentName()) {
                if (!mRealOutputDeviceName.empty())
                    setup.outputDeviceName = juce::String(mRealOutputDeviceName);
                if (!mRealInputDeviceName.empty())
                    setup.inputDeviceName = juce::String(mRealInputDeviceName);
            }
        }
#endif

        if (!deviceName.empty()) {
            setup.outputDeviceName = juce::String(deviceName);
        } else if (!mDeviceMode.empty()) {
            // Re-assert the user's explicit output choice on input-only switches
            setup.outputDeviceName = juce::String(mDeviceMode);
        }
        // "__none__" can arrive here with numInputChannels still > 0 (the
        // exclusive-pair resolver yields a carried-over patchbay input when
        // the output moves away) — it is a disable request, never a device
        // name, so it must take the disable branch below.
        if (mCurrentConfig.numInputChannels > 0 && inputDeviceName != "__none__") {
            if (!inputDeviceName.empty()) {
                // Explicit input device requested — save for future re-enable
                setup.useDefaultInputChannels = false;
                setup.inputDeviceName = juce::String(inputDeviceName);
                mLastInputDeviceName = inputDeviceName;
                juce::BigInteger inputBits;
                inputBits.setRange(0, mCurrentConfig.numInputChannels, true);
                setup.inputChannels = inputBits;
            } else {
                // Re-enable inputs — must explicitly set the input device name.
                // useDefaultInputChannels won't auto-fill the device name when
                // numInputChansNeeded was 0 at init() time (boot with -i 0).
                setup.useDefaultInputChannels = false;
                juce::BigInteger inputBits;
                inputBits.setRange(0, mCurrentConfig.numInputChannels, true);
                setup.inputChannels = inputBits;

                if (setup.inputDeviceName.isEmpty()) {
                    if (!mLastInputDeviceName.empty()) {
                        setup.inputDeviceName = juce::String(mLastInputDeviceName);
                    } else {
                        auto* currentType = mDeviceManager->getCurrentDeviceTypeObject();
                        if (currentType) {
                            auto inputNames = currentType->getDeviceNames(true);
                            if (!inputNames.isEmpty())
                                setup.inputDeviceName = inputNames[0];
                        }
                    }
                }
            }
        } else {
            // Disable inputs — save the current input device name, then release
            if (!setup.inputDeviceName.isEmpty())
                mLastInputDeviceName = setup.inputDeviceName.toStdString();
            setup.useDefaultInputChannels = false;
            setup.inputChannels.clear();
            setup.inputDeviceName = "";
        }
        // Explicitly set output bits rather than relying on
        // useDefaultOutputChannels. JUCE's "default" is derived from the
        // numOutputChannelsNeeded passed at init() time — with
        // auto-max that's kRequestMaxChannels, but under some swap
        // sequences JUCE re-evaluates and reports 0 active outputs on
        // setAudioDeviceSetup (symptom: activeOut=0 on Loopback despite
        // the device having 4 channels). Setting kRequestMaxChannels
        // bits directly lets CoreAudio clamp to the device's real
        // channel count — same pattern as the input side.
        setup.useDefaultOutputChannels = false;
        {
            juce::BigInteger outputBits;
            outputBits.setRange(0, kRequestMaxChannels, true);
            setup.outputChannels = outputBits;
        }
        if (sampleRate > 0) setup.sampleRate = sampleRate;
        if (bufferSize > 0) setup.bufferSize = bufferSize;

#ifdef __APPLE__
        // On macOS, if input and output are different devices, create an
        // Aggregate Device with drift correction instead of relying on
        // JUCE's AudioIODeviceCombiner (which has no drift correction).
        // Skip aggregation for Bluetooth/AirPlay inputs — they force
        // low-quality codec modes and don't support drift correction.
        bool wasOnAggregate = AggregateDeviceHelper::exists();
        bool needsAggregate = !setup.outputDeviceName.isEmpty()
            && !setup.inputDeviceName.isEmpty()
            && setup.outputDeviceName != setup.inputDeviceName;

        bool dropInput = false;
        if (needsAggregate) {
            // Skip the aggregate when either sub-device is unsuitable —
            // isSuitableForAggregate excludes exactly wireless (Bluetooth /
            // AirPlay: HAL can't open them and codec-mode negotiation
            // wrecks rates). Virtual devices (Loopback, BlackHole) are
            // deliberately allowed: the aggregate works when the hardware
            // sub-device is clock master (AggregateDeviceHelper's master
            // selection), and virtual-output + hardware-mic is a
            // field-verified pairing. The skip line below IS
            // user-actionable and always logs; per-device match tracing
            // was noise.
            auto devices = listDevices();
            std::string outName = setup.outputDeviceName.toStdString();
            std::string inName  = setup.inputDeviceName.toStdString();
            bool matched = false;
            for (auto& dev : devices) {
                bool nameMatch = (dev.name == outName || dev.name == inName);
                if (nameMatch) matched = true;
                if (nameMatch && !dev.isSuitableForAggregate()) {
                    needsAggregate = false;
                    dropInput = true;
                    fprintf(stderr, "[device-setup] skipping aggregate — '%s' is not "
                            "aggregable (wireless or aggregate-class); input disabled\n",
                            dev.name.c_str());
                    fflush(stderr);
                    break;
                }
            }
            if (!matched) {
                fprintf(stderr, "[agg-filter] WARNING: no device matched outName='%s' inName='%s' "
                        "— filter never fired\n", outName.c_str(), inName.c_str());
                fflush(stderr);
            }
        }

        if (needsAggregate) {
            // Remember the real device names before replacing with aggregate
            mRealOutputDeviceName = setup.outputDeviceName.toStdString();
            mRealInputDeviceName  = setup.inputDeviceName.toStdString();

            // Pause CFRunLoop pumping to prevent JUCE's audioDeviceListChanged
            // from firing during aggregate destroy/create — it crashes trying
            // to reinitialise with a stale device reference.
            runLoopSuppress.arm();
            // Pass the engine's current sample rate so the aggregate's
            // sub-devices are forced to the same rate — otherwise
            // CoreAudio will apply aggregate-level SRC inside the
            // IOProc to bridge a rate mismatch, producing "hideous
            // distortion" (user-reported symptom).
            double wantedRate = sampleRate > 0
                ? sampleRate
                : static_cast<double>(mCurrentConfig.sampleRate);
            double aggRate = 0;
            auto aggName = AggregateDeviceHelper::createOrUpdate(
                mRealOutputDeviceName, mRealInputDeviceName, wantedRate, &aggRate);
            if (!aggName.empty()) {
                // Open the aggregate at the rate it actually settled on. If
                // the sub-devices refused wantedRate the helper adopted their
                // current rate; forcing wantedRate here would re-introduce the
                // SRC mismatch (distortion).
                if (aggRate > 0) setup.sampleRate = aggRate;
                // Use the aggregate as a single device for both I/O
                setup.outputDeviceName = juce::String(aggName);
                setup.inputDeviceName  = juce::String(aggName);
                clampAggregateBufferIfNeeded(setup.bufferSize);

                // Re-issue the input bitmask with an offset past the
                // output sub-device's own input streams (e.g. Loopback
                // Audio exposes 4 loopback-return inputs; without this
                // offset JUCE activates those silent returns instead of
                // the user's intended input device's channels).
                if (mCurrentConfig.numInputChannels > 0) {
                    const int inOffset = aggregateInputChannelOffsetFor(mRealOutputDeviceName);
                    if (inOffset > 0) {
                        juce::BigInteger inputBits;
                        inputBits.setRange(inOffset,
                                           mCurrentConfig.numInputChannels, true);
                        setup.inputChannels = inputBits;
                        fprintf(stderr, "[device-setup] aggregate input bits offset by %d "
                                "(output sub-device '%s' contributes %d input channels) — "
                                "active input range = [%d..%d]\n",
                                inOffset, mRealOutputDeviceName.c_str(), inOffset,
                                inOffset, inOffset + mCurrentConfig.numInputChannels - 1);
                        fflush(stderr);
                    }
                }

                // Wait until JUCE can actually see the new aggregate before
                // the setAudioDeviceSetup below opens it — a fixed sleep races
                // CoreAudio's device-list refresh and errors "No such device".
                waitForDeviceVisible(aggName, 2000);
            }
        } else {
            // Same device for both I/O, or no input — no aggregate needed.
            // Save the real input device name before clearing — when switching
            // to AirPlay (no aggregate), we want to restore the input device
            // when switching back to local speakers.
            if (!mRealInputDeviceName.empty())
                mLastInputDeviceName = mRealInputDeviceName;
            mRealOutputDeviceName.clear();
            mRealInputDeviceName.clear();

            // If we skipped aggregate because a sub-device is unsuitable
            // (wireless), drop the input. Keeping it would make JUCE fall
            // back to its combiner, which has the same failure mode as
            // our aggregate on wireless (both use AudioUnitRender under
            // the hood).
            if (dropInput && !setup.inputDeviceName.isEmpty()) {
                fprintf(stderr, "[device-setup] clearing input (was '%s') because output "
                        "can't be combined with it\n",
                        setup.inputDeviceName.toRawUTF8());
                fflush(stderr);
                mLastInputDeviceName = setup.inputDeviceName.toStdString();
                setup.inputDeviceName = "";
                setup.inputChannels.clear();
                inputWasDropped = true;
            }

            // Don't destroy aggregate yet — JUCE still references it.
            // setAudioDeviceSetup below will switch JUCE to the new device,
            // then we destroy the orphaned aggregate safely.
        }
#endif

        // Don't call closeAudioDevice() here — it races with JUCE's internal
        // CoreAudio lock on destruction of aggregates that contained virtual
        // sub-devices (_os_unfair_lock_unowned_abort). We rely on each new
        // aggregate having a unique name (see AggregateDeviceHelper) so
        // JUCE's setAudioDeviceSetup sees it as a different device and
        // reopens properly.

        fprintf(stderr, "[device-setup] calling setAudioDeviceSetup: out='%s' in='%s' sr=%.0f buf=%d\n",
                setup.outputDeviceName.toRawUTF8(),
                setup.inputDeviceName.toRawUTF8(),
                setup.sampleRate, setup.bufferSize);
        fflush(stderr);
        juce::String err = mDeviceManager->setAudioDeviceSetup(setup, true);
        fprintf(stderr, "[device-setup] setAudioDeviceSetup returned: '%s'\n",
                err.isEmpty() ? "OK" : err.toRawUTF8());
        fflush(stderr);
        if (err.isNotEmpty()) errStr = err.toStdString();

        // Input-fallback: the setup failed while an input was requested
        // (Windows mic privacy denied, exclusive-mode contention, …) —
        // retry with the input cleared and let the RETRY attribute the
        // fault: success means the input was the problem (output keeps
        // working, prefs show an empty input instead of the whole rate
        // change rolling back into a cold-swap rebuild loop); failure
        // propagates the original class of error. This used to guess the
        // faulty side by substring-matching the device name inside JUCE's
        // error text, which broke silently whenever the wording changed.
        if (!errStr.empty() && setup.inputDeviceName.isNotEmpty()) {
            const std::string firstError = errStr;
            const std::string failedInputName = setup.inputDeviceName.toStdString();
            const std::string pairedOutputName = setup.outputDeviceName.toStdString();
            juce::AudioDeviceManager::AudioDeviceSetup outOnly = setup;
            outOnly.inputDeviceName = juce::String();
            outOnly.useDefaultInputChannels = false;
            outOnly.inputChannels.clear();
            fprintf(stderr,
                    "[device-setup] input '%s' failed when paired with output '%s' "
                    "(%s) — retrying output-only\n",
                    failedInputName.c_str(), pairedOutputName.c_str(),
                    firstError.c_str());
            fflush(stderr);
            juce::String retryErr = mDeviceManager->setAudioDeviceSetup(outOnly, true);
            if (retryErr.isEmpty()) {
                setup = outOnly;
                errStr.clear();
                result.inputUnavailable = true;
                result.inputUnavailableReason = firstError;
                // Remember the (output, input) pair as known-bad so
                // sendDeviceReport hides this input from the dropdown
                // while pairedOutputName is the active output. Per-
                // output scoping: the same input can pair fine with a
                // different output (typical with WASAPI Shared vs
                // ASIO on the same hardware).
                {
                    std::lock_guard<std::mutex> lock(mUngatableInputPairsMutex);
                    mUngatableInputPairs.emplace(pairedOutputName, failedInputName);
                }
            } else {
                errStr = retryErr.toStdString();
                fprintf(stderr,
                        "[device-setup] output-only retry also failed: %s\n",
                        errStr.c_str());
                fflush(stderr);
            }
        }

#ifdef __APPLE__
        // Now JUCE has switched away from the old aggregate — safe to
        // destroy it. AggregateDeviceHelper stashes the previous ID in
        // sPrevAggregateID precisely so this happens after JUCE has moved.
        AggregateDeviceHelper::destroyPrevious();
        juce::Thread::sleep(150);
        // Also destroy the current one if we're no longer using an aggregate
        // (e.g. single-device setup that doesn't need input combining).
        if (wasOnAggregate && !needsAggregate) {
            AggregateDeviceHelper::destroy();
            juce::Thread::sleep(150);
        }
        // mSuppressRunLoop clears via runLoopSuppress at function exit —
        // exception-proof, and a marginally longer suppression window is
        // the safe direction.
#endif
    } else {
        // Headless: no real device to configure; use failure hook for testing.
        // Mirrors the real-device input-fallback above so the same code path
        // can be exercised by unit tests via the testSwapFailure hook.
        if (testSwapFailure) {
            const bool inputRequested = !inputDeviceName.empty();
            errStr = testSwapFailure(inputRequested);
            // Same retry-attributes-the-fault contract as the real path.
            if (!errStr.empty() && inputRequested) {
                const std::string firstError = errStr;
                std::string retryErr = testSwapFailure(false);
                if (retryErr.empty()) {
                    errStr.clear();
                    result.inputUnavailable = true;
                    result.inputUnavailableReason = firstError;
                } else {
                    errStr = retryErr;
                }
            }
        }
    }

    if (!errStr.empty()) {
        if (isCold) { rebuild_world(currentRate); mWorldRebuilt = true; }
        // --- Restart audio (failure path) ---
        if (mDeviceManager) {
            // Restore the previous device setup. A failed setAudioDeviceSetup
            // typically leaves the manager with no bound device. If we can't
            // restore (and the default-device fallback below also fails),
            // startAudioSource() sees no current device and brings up the
            // headless driver so the engine stays responsive (Spider /done
            // syncs return) instead of silently dead with the audio thread
            // never ticking.
            fprintf(stderr,
                    "[device-setup] swap failed (%s), restoring previous setup: out='%s' in='%s' sr=%.0f buf=%d\n",
                    errStr.c_str(),
                    prevSetup.outputDeviceName.toRawUTF8(),
                    prevSetup.inputDeviceName.toRawUTF8(),
                    prevSetup.sampleRate, prevSetup.bufferSize);
            fflush(stderr);
            juce::String restoreErr = mDeviceManager->setAudioDeviceSetup(prevSetup, true);
            if (restoreErr.isNotEmpty()) {
                fprintf(stderr,
                        "[device-setup] WARNING: failed to restore previous setup: %s\n",
                        restoreErr.toRawUTF8());
                fflush(stderr);
                // Last-resort recovery: try the system default with output-only.
                // If even this fails, startAudioSource() will choose the headless
                // fallback (no current device, so Headless) and the engine
                // stays up; the next user-driven swap can take it from there.
                juce::String fallbackErr =
                    mDeviceManager->initialiseWithDefaultDevices(0, mCurrentConfig.numOutputChannels);
                if (fallbackErr.isNotEmpty()) {
                    fprintf(stderr,
                            "[device-setup] WARNING: default-device fallback also failed: %s, "
                            "engine will run via headless driver\n",
                            fallbackErr.toRawUTF8());
                    fflush(stderr);
                } else {
                    fprintf(stderr,
                            "[device-setup] recovered to system default after rollback failure\n");
                    fflush(stderr);
                    // Clear aggregate-bookkeeping; we're on a single device now.
                    mRealOutputDeviceName.clear();
                    mRealInputDeviceName.clear();
                    // Drop input — fallback is output-only. Caller can
                    // re-enable inputs explicitly afterward.
                    mCurrentConfig.numInputChannels = 0;
                    uint32_t* opts = reinterpret_cast<uint32_t*>(
                        sp_arena() + WORLD_OPTIONS_START);
                    opts[sonicpi::WorldOpts::kNumInputBusChannels] = 0;
                }
            }
        }
        readerPark.resumeNow();
        startAudioSource();
        mAudioCallback.resume();
        if (isCold) mSampleLoader.resumeLoading();
        result.error = errStr;
        if (isCold) setEngineState(EngineState::Running, "swap-failed-rollback");
        if (onSwapEvent) onSwapEvent("swap:failed", result);
        return result;
    }

    if (isCold) {
        double newRate = (sampleRate > 0) ? sampleRate : currentRate;
        if (mDeviceManager) {
            auto* newDev = mDeviceManager->getCurrentAudioDevice();
            newRate = newDev ? newDev->getCurrentSampleRate() : newRate;
        }
        mCurrentConfig.sampleRate = static_cast<int>(newRate);

        uint32_t* opts = reinterpret_cast<uint32_t*>(sp_arena() + WORLD_OPTIONS_START);
        opts[sonicpi::WorldOpts::kSampleRate] = static_cast<uint32_t>(newRate);

        // Update the world's input/output bus counts to match the new
        // device. rebuild_world() reads these to size the scsynth
        // World's audio buses; without the update the rebuilt World
        // stays at the boot-time channel count, so Out.ar to higher
        // buses (e.g. Out.ar(2, sig) with 4-channel Loopback) lands on
        // internal private buses instead of hardware.
        if (mDeviceManager) {
            if (auto* dev = mDeviceManager->getCurrentAudioDevice()) {
                int newOut = dev->getActiveOutputChannels().countNumberOfSetBits();
                int newIn  = dev->getActiveInputChannels().countNumberOfSetBits();
                if (newOut > 0) {
                    mCurrentConfig.numOutputChannels = newOut;
                    opts[sonicpi::WorldOpts::kNumOutputBusChannels]
                        = static_cast<uint32_t>(newOut);
                }
                // Respect inputWasDropped: when we've dropped input because
                // the new output can't be aggregated (wireless / virtual),
                // keep the previously-remembered input count in config but
                // tell the world there are zero inputs for this rebuild.
                if (!inputWasDropped || newIn > 0) {
                    mCurrentConfig.numInputChannels = newIn;
                }
                opts[sonicpi::WorldOpts::kNumInputBusChannels]
                    = static_cast<uint32_t>(inputWasDropped ? 0 : newIn);
            }
        }

        try {
            if (testRebuildFailure) {
                std::string failMsg = testRebuildFailure();
                if (!failMsg.empty())
                    throw std::runtime_error(failMsg);
            }
            rebuild_world(newRate);
            mWorldRebuilt = true;
        } catch (const std::exception& e) {
            fprintf(stderr, "[supersonic] rebuild_world failed: %s — recovering with safe defaults\n",
                    e.what());
            fflush(stderr);

            double safeRate = currentRate;
            int safeBuffer = 128;
            opts[sonicpi::WorldOpts::kSampleRate] = static_cast<uint32_t>(safeRate);
            mCurrentConfig.sampleRate = static_cast<int>(safeRate);
            mCurrentConfig.bufferSize = safeBuffer;

            try {
                rebuild_world(safeRate);
                mWorldRebuilt = true;
                recovered = true;
                result.error = std::string("rebuild failed (") + e.what()
                             + "), recovered at safe defaults";
                result.sampleRate = safeRate;
                result.bufferSize = safeBuffer;
            } catch (const std::exception& e2) {
                fprintf(stderr, "[supersonic] rebuild recovery ALSO failed: %s\n", e2.what());
                fflush(stderr);
                result.error = std::string("rebuild failed and recovery failed: ") + e2.what();
                setEngineState(EngineState::Error, "rebuild-failed");
                if (onSwapEvent) onSwapEvent("swap:failed", result);
                return result;
            }
        }
    }

    // --- Restart audio (success path) ---
    readerPark.resumeNow();
    startAudioSource();
    mAudioCallback.resume();

    if (isCold) {
        // Don't restore synthdefs, buffers, or module state here.
        // The client (Spider) receives /supersonic/setup and handles
        // all reinitialisation — reloading synthdefs, clearing sample
        // caches, recreating groups/mixer/scope.  Restoring from the
        // StateCache would create duplicate state and cause distortion.
        mSampleLoader.resumeLoading();
    }

    if (mDeviceManager) {
        auto* finalDev = mDeviceManager->getCurrentAudioDevice();
        if (finalDev) {
            result.sampleRate = finalDev->getCurrentSampleRate();
            result.bufferSize = finalDev->getCurrentBufferSizeSamples();
            mCurrentConfig.sampleRate = static_cast<int>(result.sampleRate);
            mCurrentConfig.numOutputChannels = finalDev->getActiveOutputChannels().countNumberOfSetBits();
            // Preserve the user's desired input channel count when we had to
            // drop inputs for an unsuitable output (wireless/virtual). Without
            // this, a detour through e.g. AirPlay would permanently erase the
            // mic setting — switching back to speakers wouldn't re-aggregate.
            int actualIn = finalDev->getActiveInputChannels().countNumberOfSetBits();
            if (!inputWasDropped || actualIn > 0) {
                mCurrentConfig.numInputChannels = actualIn;
            }

            juce::AudioDeviceManager::AudioDeviceSetup finalSetup;
            mDeviceManager->getAudioDeviceSetup(finalSetup);
            result.inputDeviceName = finalSetup.inputDeviceName.toStdString();

            // Report the device that actually opened, not the request:
            // JUCE keeps the requested name in its setup even when the
            // device type resolved it elsewhere, and subscribers (the GUI)
            // must never be told a fiction. On macOS the aggregate wraps
            // the real device — report the real one, matching the device
            // lists the GUI displays.
            result.deviceName = mRealOutputDeviceName.empty()
                ? finalDev->getName().toStdString()
                : mRealOutputDeviceName;

            fprintf(stderr, "[device-setup] switched to %s: %s %.0fHz buf=%d %dch\n",
                    finalDev->getTypeName().toRawUTF8(),
                    finalDev->getName().toRawUTF8(),
                    result.sampleRate, result.bufferSize,
                    mCurrentConfig.numOutputChannels);

#ifdef __APPLE__
            // Remember the rate of the last non-wireless settle so a
            // future detour through AirPlay/Bluetooth doesn't leave the
            // engine stuck at the wireless receiver's negotiated rate.
            if (result.sampleRate > 0) {
                bool finalIsWireless = false;
                std::string finalName = mRealOutputDeviceName.empty()
                    ? finalDev->getName().toStdString()
                    : mRealOutputDeviceName;
                for (auto& d : listDevices(false)) {
                    if (sameDeviceName(d.name, finalName) && d.isWirelessTransport()) {
                        finalIsWireless = true;
                        break;
                    }
                }
                if (!finalIsWireless)
                    mPreWirelessRate = static_cast<int>(result.sampleRate);
            }
#endif
        }
    } else {
        if (!recovered) {
            result.sampleRate = isCold ? sampleRate : currentRate;
        }
        result.bufferSize = mCurrentConfig.bufferSize;
    }
    result.success = true;
    recordSwapPreferences(deviceName, inputDeviceName, result.sampleRate, origin);
    if (isCold) {
        if (recovered) {
            setEngineState(EngineState::Running, "swap-recovered");
            if (onSwapEvent) onSwapEvent("swap:recovered", result);
        } else {
            setEngineState(EngineState::Running, "rate-change");
            if (onSwapEvent) onSwapEvent("swap:complete", result);
        }
    } else {
        if (onSwapEvent) onSwapEvent("swap:complete", result);
    }
    fprintf(stderr, "[switchDevice] EXIT success=%d type=%s sr=%.0f buf=%d out=%d in=%d err='%s'\n",
            result.success ? 1 : 0,
            (result.type == SwapType::Cold) ? "Cold" : "Hot",
            result.sampleRate, result.bufferSize,
            mCurrentConfig.numOutputChannels, mCurrentConfig.numInputChannels,
            result.error.c_str());
    fflush(stderr);
    printDeviceList();
    return result;
}

void SupersonicEngine::teardownDeviceManager() {
    if (mDeviceManager) {
        mDeviceManager->removeChangeListener(this);
        mDeviceManager->removeAudioCallback(&mAudioCallback);
        mDeviceManager->closeAudioDevice();
        mDeviceManager.reset();
    }
}

bool SupersonicEngine::runOnMessageThread(std::function<void()> fn, int timeoutMs) {
    auto* mm = juce::MessageManager::getInstanceWithoutCreating();
    if (mm == nullptr || mm->isThisTheMessageThread()) {
        fn();
        return true;
    }

    // `claimed` decides who runs fn — exactly one of the queued lambda and the
    // timeout fallback below. shared_ptr keeps the state alive if the queued
    // lambda fires after this function has already returned.
    struct Task {
        std::function<void()> fn;
        juce::WaitableEvent   done;
        std::atomic<bool>     claimed{false};
    };
    auto task = std::make_shared<Task>();
    task->fn = std::move(fn);
    juce::MessageManager::callAsync([task] {
        if (!task->claimed.exchange(true)) {
            task->fn();
            task->done.signal();
        }
    });
    if (task->done.wait(timeoutMs)) return true;

    if (!task->claimed.exchange(true)) {
        // The queue never ran it — no pump on the message thread. Run inline.
        task->fn();
        return false;
    }
    // Lost the claim: fn is mid-run on the message thread — wait it out.
    task->done.wait(-1);
    return true;
}

juce::String SupersonicEngine::recreateDeviceManager() {
    // Release the stale, hibernate-killed CoreAudio/HAL client completely, then
    // build a fresh manager. A reopen keeps this same dead connection; only a
    // new manager gets a new IsolatedCoreAudioClient + AudioObjectIDs that
    // coreaudiod will actually drive with a live IO thread. Runs on the device task lane
    // (the recovery worker — see requestAudioRecovery), holding the swap gate.
#ifdef _WIN32
    // The old manager's DeviceChangeDetector hidden windows are owned by the
    // message thread (created there at boot). DestroyWindow from this worker
    // fails silently cross-thread, leaving a zombie window whose GWLP_USERDATA
    // points at the freed detector — the next WM_DEVICECHANGE broadcast
    // (resume, hot-plug) then use-after-frees in Timer::startTimer. Tear down
    // AND rebuild on the message thread so window ownership stays with the
    // pump. Device types are created there too (lazily created on first
    // touch, they'd otherwise land on this worker, which exits after
    // recovery — killing hot-plug detection). Deadlock-safe: message-thread
    // device handlers only try_lock the swap gate we hold. The message-thread
    // work is fast (close + construct + enumerate); the open that follows
    // stays on this worker.
    constexpr int kRecreateOnMessageThreadTimeoutMs = 4000;
    const bool marshalled = runOnMessageThread([this]() {
        try {
            teardownDeviceManager();
            mDeviceManager = makeDeviceManager();
            // A fresh manager defaults to WASAPI (JUCE's first type); restore
            // the boot-time driver choice (see init) so recovery doesn't
            // silently change driver. Skipped under the factory seam — an
            // injected manager owns its own types.
            if (!mCurrentConfig.deviceManagerFactory) {
                const std::string bootType =
                    !mBootDriver.empty() ? mBootDriver
                                         : std::string("DirectSound");
                for (auto* t : mDeviceManager->getAvailableDeviceTypes()) {
                    if (t->getTypeName().toStdString() == bootType) {
                        mDeviceManager->setCurrentAudioDeviceType(
                            juce::String(bootType), true);
                        break;
                    }
                }
            }
        } catch (...) {
            ss_log("[recover] device manager rebuild threw on message thread");
        }
    }, kRecreateOnMessageThreadTimeoutMs);
    if (!marshalled)
        ss_log("[recover] message thread unresponsive — device manager rebuilt "
               "on the recovery worker (hidden-window ownership degraded)");
    if (!mDeviceManager)
        return "recreate: device manager rebuild failed";
#else
    teardownDeviceManager();
    mDeviceManager = makeDeviceManager();
#if defined(__linux__) && defined(SUPERSONIC_PIPEWIRE)
    // Same registration and preference as at boot; the scan must land before
    // the default-device init below so the fresh manager can see PipeWire's
    // devices at all. Skipped under the factory seam. A boot driver other
    // than PipeWire (an honoured --audio-driver) is restored instead of
    // re-applying the PipeWire preference — recovery must not change the
    // driver the user chose.
    if (!mCurrentConfig.deviceManagerFactory) {
        registerPipeWireDriver(*mDeviceManager);
        if (mBootDriver.empty() || mBootDriver == "PipeWire") {
            preferPipeWireDriverIfAvailable(*mDeviceManager);
        } else {
            mDeviceManager->setCurrentAudioDeviceType(
                juce::String(mBootDriver), true);
        }
    }
#endif
#endif

    // Open the device via the shared system-default reinit: it preserves the
    // session sample rate, drops the now-stale aggregate / real-device names,
    // and cleans up leftover aggregates — so the engine's cached device state
    // stays consistent with what actually opened and a later reopen can't chase
    // a dead aggregate.
    juce::String err = reinitialiseWithDefaultsPreservingConfig();
    if (err.isNotEmpty()) return err;
    if (!mDeviceManager->getCurrentAudioDevice())
        return "recreate: opened no device";

    // Device is open but no callback/listener is attached yet — the caller
    // (recoverAudio) promotes to it via startAudioSource() once it's confirmed
    // this fresh connection actually ticks. Recovery falls back to the default
    // output with no inputs (a pinned device / mic is not restored), but the
    // cache cleared above keeps the rest of the engine consistent with that.
    return {};
}

SwapResult SupersonicEngine::reopenCurrentDevice() {
    SwapResult result;

    if (!mDeviceManager) {
        result.error = "no audio device manager (headless)";
        return result;
    }

    // Always delegate to switchDevice with forceCold=true so aggregates
    // get rebuilt with the same sub-device pair (and pick up any fresh
    // channel-count change). System / manual / aggregate all share this
    // path — mRealOutputDeviceName / mRealInputDeviceName track sub-
    // device names behind an aggregate and are the right thing to pass
    // whether we're in system mode or pinned to an explicit selection.
    // For direct (non-aggregate) devices those fields are empty and we
    // fall back to the JUCE device name.
    std::string outName = mRealOutputDeviceName;
    std::string inName  = mRealInputDeviceName;
    if (outName.empty()) {
        if (auto* dev = mDeviceManager->getCurrentAudioDevice())
            outName = dev->getName().toStdString();
    }
    if (outName.empty()) {
        result.error = "no current output device to reopen";
        return result;
    }
    fprintf(stderr, "[reopen] forceCold switch out='%s' in='%s' mode='%s'\n",
            outName.c_str(), inName.c_str(),
            mDeviceMode.empty() ? "system" : mDeviceMode.c_str());
    fflush(stderr);
    return switchDevice(outName, 0, 0, /*forceCold=*/true, inName,
                        SwapOrigin::Internal);
}

// --- Input channel management ---

SwapResult SupersonicEngine::enableInputChannels(int numChannels) {
    // Resolve which input device we mean BEFORE the channel width, so the
    // width can be clamped against that device's probed capacity. When
    // enabling, prefer an explicit name over letting switchDevice fall
    // back to "first in JUCE's input list" — that fallback can pick a
    // virtual device (e.g. NDI Audio) over the real hardware mic,
    // producing silent zeros. Prefer, in order: saved
    // mLastInputDeviceName, the macOS system default input, then an empty
    // string (switchDevice falls back).
    std::string inputName;
    const char* inputSource = "disable";
    if (numChannels != 0) {
        inputName = mLastInputDeviceName;
        inputSource = inputName.empty() ? "none" : "mLastInputDeviceName";
    }
#ifdef __APPLE__
    if (numChannels != 0 && inputName.empty()) {
        AudioObjectPropertyAddress addr = {
            kAudioHardwarePropertyDefaultInputDevice,
            kAudioObjectPropertyScopeGlobal,
            kAudioObjectPropertyElementMain
        };
        AudioDeviceID devId = 0;
        UInt32 sz = sizeof(devId);
        if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &addr, 0, nullptr, &sz, &devId) == noErr
            && devId != 0) {
            CFStringRef cfName = nullptr;
            UInt32 nsz = sizeof(cfName);
            AudioObjectPropertyAddress nameAddr = {
                kAudioDevicePropertyDeviceNameCFString,
                kAudioObjectPropertyScopeGlobal,
                kAudioObjectPropertyElementMain
            };
            if (AudioObjectGetPropertyData(devId, &nameAddr, 0, nullptr, &nsz, &cfName) == noErr && cfName) {
                char buf[256];
                CFStringGetCString(cfName, buf, sizeof(buf), kCFStringEncodingUTF8);
                CFRelease(cfName);
                inputName = buf;
                inputSource = "kAudioHardwarePropertyDefaultInputDevice";
            }
        }
    }
#endif

    // -1 means "re-enable inputs": resolved against the boot -i flag and
    // clamped to the device's probed input capacity. The clamp is what
    // keeps the auto-max request off WASAPI, which rejects
    // setAudioDeviceSetup outright when asked for more inputs than exist
    // (CoreAudio silently clamps instead, which is how the unclamped
    // path went unnoticed on mac).
    if (numChannels != 0) {
        int probed = -1;
        if (mDeviceManager && !inputName.empty())
            probed = probeDeviceChannelCount(inputName, true,
                                             probeDriverTypeName());
        numChannels = sonicpi::device::resolveInputWidth(
            numChannels, mBootInputChannels, probed);
    }

    // Check if this is actually a change
    if (numChannels == mCurrentConfig.numInputChannels) {
        SwapResult result;
        result.success = true;
        result.type = SwapType::Hot;  // no-op
        result.sampleRate = mCurrentConfig.sampleRate;
        result.bufferSize = mCurrentConfig.bufferSize;
        return result;
    }

    // Refuse "disable inputs" on ASIO. ASIO drivers are full-duplex
    // single-device by spec — one stream owns both directions.
    // Reconfiguring with input=0 while keeping output crashes real
    // drivers (MOTU Pro Audio observed). Output-only is served by
    // switching driver to Windows Audio / DirectSound.
    if (numChannels == 0 && mDeviceManager) {
        if (auto* dev = mDeviceManager->getCurrentAudioDevice()) {
            if (dev->getTypeName().toStdString() == "ASIO") {
                SwapResult result;
                result.error = "Cannot disable input on ASIO — ASIO drivers "
                               "are full-duplex by spec. Switch driver to "
                               "Windows Audio / DirectSound to run output-only.";
                fprintf(stderr, "[enable-inputs] refusing disable on ASIO "
                        "(would crash the driver)\n");
                fflush(stderr);
                return result;
            }
        }
    }

    // Save old values for rollback on failure
    int oldNumInputChannels = mCurrentConfig.numInputChannels;
    uint32_t* opts = reinterpret_cast<uint32_t*>(sp_arena() + WORLD_OPTIONS_START);
    uint32_t oldNumInputBusChannelsOpt =
        opts[sonicpi::WorldOpts::kNumInputBusChannels];

    // Update config and worldOptions before the cold swap
    mCurrentConfig.numInputChannels = numChannels;
    opts[sonicpi::WorldOpts::kNumInputBusChannels] = static_cast<uint32_t>(numChannels);

    fprintf(stderr, "[enable-inputs] resolved input='%s' (source=%s) channels=%d\n",
            inputName.c_str(), inputSource, numChannels);
    fflush(stderr);
    // For disable, pass __none__ sentinel so switchDevice takes the disable
    // path (clears setup.inputDeviceName + inputChannels) instead of trying
    // to treat an empty string as "re-enable with last known input".
    auto result = switchDevice("", 0, 0, true, numChannels > 0 ? inputName : std::string("__none__"));

    if (!result.success) {
        mCurrentConfig.numInputChannels = oldNumInputChannels;
        opts[sonicpi::WorldOpts::kNumInputBusChannels] = oldNumInputBusChannelsOpt;
    }

    return result;
}

// --- Audio driver management ---

std::vector<std::string> SupersonicEngine::listDrivers() const {
    // Serialise mDeviceManager access against device mutations / recovery's
    // recreate (see mSwapMutex). Recursive: mutation paths call this under the
    // gate. Lock order is always gate-then-mListDriversMutex.
    std::lock_guard<std::recursive_mutex> gate(mSwapMutex);
    if (!mDeviceManager) return {};

    // Cache hit — skip the rescan. sendDeviceReport() is called many
    // times during boot (notify registration, first info push, device
    // change settles, aggregate build) and once per user-initiated
    // switch; re-running scanForDevices() on every call is wasteful
    // and on Linux without a JACK server produces libjack connect()
    // stderr spam. Short TTL so a freshly-started jackd shows up.
    {
        std::lock_guard<std::mutex> lk(mListDriversMutex);
        auto now = std::chrono::steady_clock::now();
        if (!mCachedDrivers.empty()
            && (now - mCachedDriversAt) < std::chrono::seconds(3))
            return mCachedDrivers;
    }

    std::vector<std::string> result;
    auto& types = mDeviceManager->getAvailableDeviceTypes();
    for (auto* type : types) {
        // JUCE registers every compiled-in device type regardless of
        // runtime availability (e.g. JackAudioIODeviceType appears in
        // getAvailableDeviceTypes() whenever JUCE_JACK=1, even if no
        // jackd / pipewire-jack server is running). Offering a driver
        // the user can't switch to is worse than hiding it, so rescan
        // and only advertise drivers that enumerate at least one output
        // device right now. ALSA / CoreAudio / WASAPI always have the
        // hardware on their side so they pass; JACK / ASIO only show
        // when a server or driver is actually reachable.
        type->scanForDevices();
        auto names = type->getDeviceNames(false);
        if (names.isEmpty()) continue;

        // Same rule as listDevices: an ASIO driver the loader rejects can
        // never be opened, so it doesn't count towards the type being
        // reachable. When every installed ASIO driver is unloadable (e.g.
        // x64-only drivers on an ARM64 host) the whole type is hidden
        // rather than offering a driver with nothing selectable under it.
        const std::string typeName = type->getTypeName().toStdString();
        if (typeName == "ASIO") {
            const auto unloadable = sonicpi::device::unloadableAsioDrivers();
            bool anyUsable = false;
            for (const auto& n : names) {
                if (!unloadable.count(n.toStdString())) { anyUsable = true; break; }
            }
            if (!anyUsable) continue;
        }

        result.push_back(typeName);
    }

    {
        std::lock_guard<std::mutex> lk(mListDriversMutex);
        mCachedDrivers = result;
        mCachedDriversAt = std::chrono::steady_clock::now();
    }
    return result;
}

std::string SupersonicEngine::currentDriver() const {
    // Serialise mDeviceManager access against device mutations / recovery's
    // recreate (see mSwapMutex). Recursive: mutation paths call this under gate.
    std::lock_guard<std::recursive_mutex> gate(mSwapMutex);
    if (!mDeviceManager) return "";
    if (auto* dev = mDeviceManager->getCurrentAudioDevice())
        return dev->getTypeName().toStdString();
    // No device open: fall back to the active type so the GUI's
    // driver dropdown stays on the right entry instead of going
    // blank. The type can be set without a device during cross-
    // driver swaps and after open failures.
    return mDeviceManager->getCurrentAudioDeviceType().toStdString();
}

std::string SupersonicEngine::intendedDriver() const {
    return mIntendedDriver;
}

bool SupersonicEngine::isInputKnownBadFor(const std::string& outputName,
                                          const std::string& inputName) const {
    if (outputName.empty() || inputName.empty()) return false;
    std::lock_guard<std::mutex> lock(mUngatableInputPairsMutex);
    return mUngatableInputPairs.count({outputName, inputName}) > 0;
}

SwapResult SupersonicEngine::switchDriver(const std::string& driverName) {
    SwapResult result;
    result.deviceName = driverName;

    // ── Real-driver path ────────────────────────────────────────────────
    // Always carry an explicit device name into setAudioDeviceSetup —
    // never let JUCE's insertDefaultDeviceNames pick alphabetical-first
    // for the new type. On Windows ASIO that's the registered-but-
    // unplugged-driver hang hazard (IASIO::init() can block in COM);
    // on every driver it's a quiet UX surprise (the device dropdown
    // shows one thing, the audio is routed through another).
    if (mDeviceManager) {
        // (a) Saved per-driver preference → delegate to switchDevice
        //     with the remembered name. switchDevice's cross-driver
        //     path moves JUCE atomically.
        auto pref = mPreferredDeviceByDriver.find(driverName);
        if (pref != mPreferredDeviceByDriver.end() && !pref->second.empty()) {
            fprintf(stderr, "[device-setup] switchDriver('%s'): delegating to "
                    "switchDevice('%s') (saved preference)\n",
                    driverName.c_str(), pref->second.c_str());
            fflush(stderr);
            mIntendedDriver = driverName;
            return switchDevice(pref->second);
        }

        // (b) No saved preference, non-ASIO driver with at least one
        //     device visible → pick the driver's system-default device
        //     and delegate. Keeps the transition atomic (one cold swap)
        //     and avoids leaving the GUI in a "driver=X but no device"
        //     limbo for drivers that have a sensible default.
        if (driverName != "ASIO") {
            auto& types = mDeviceManager->getAvailableDeviceTypes();
            for (auto* type : types) {
                if (type->getTypeName().toStdString() != driverName) continue;
                type->scanForDevices();
                auto names = type->getDeviceNames(false);
                if (names.isEmpty()) break;  // fall through to (c)
                int idx = type->getDefaultDeviceIndex(false);
                if (idx < 0 || idx >= names.size()) idx = 0;
                std::string defaultName = names[idx].toStdString();
                fprintf(stderr, "[device-setup] switchDriver('%s'): no saved pref, "
                        "auto-selecting default '%s'\n",
                        driverName.c_str(), defaultName.c_str());
                fflush(stderr);
                mIntendedDriver = driverName;
                return switchDevice(defaultName);
            }
        }

        // (c) ASIO with no saved preference, or any driver with no
        //     visible devices → don't touch JUCE. setCurrentAudio-
        //     DeviceType + initialiseWithDefaultDevices stops the
        //     audio callback, so scsynth stops ticking; any user-code
        //     call hitting trigger_synth before a follow-up
        //     switchDevice would hang on /n_go. Record intent and
        //     wait for the caller's explicit device pick.
        mIntendedDriver                 = driverName;
        result.success                  = true;
        result.requiresDeviceSelection  = true;
        fprintf(stderr, "[device-setup] switchDriver('%s'): intent recorded, "
                "no device opened — caller must follow with switchDevice\n",
                driverName.c_str());
        fflush(stderr);
        if (onSwapEvent) onSwapEvent("swap:complete", result);
        return result;
    }

    // ── Headless mode ───────────────────────────────────────────────────
    // No real audio driver to switch. If the test hook is set, simulate
    // the rate the new driver's default device would report.
    if (!testDriverSwitchRate) {
        result.error = "no audio device in headless mode";
        return result;
    }
    double newRate = testDriverSwitchRate();
    if (static_cast<int>(newRate) == mCurrentConfig.sampleRate) {
        result.success    = true;
        result.type       = SwapType::Hot;
        result.sampleRate = newRate;
        result.bufferSize = mCurrentConfig.bufferSize;
        if (onSwapEvent) onSwapEvent("swap:start", result);
        if (onSwapEvent) onSwapEvent("swap:complete", result);
        return result;
    }
    return switchDevice("", newRate);
}

// --- Device change detection ---

void SupersonicEngine::changeListenerCallback(juce::ChangeBroadcaster* source) {
    if (!mRunning.load()) return;

    // Two separate windows, checked independently:
    //  - engine self-triggered mutations (shared stamp — a swap/reinit/boot
    //    just churned the device state; this event is our own echo);
    //  - our own recent processing (private stamp — JUCE fires several
    //    broadcasts per hot-plug; one pass per second is plenty).
    // Only the PRIVATE stamp is written here: writing the shared one on an
    // external event poisoned the default-follow handler's quiet window.
    const auto now = std::chrono::steady_clock::now();
    if (now - mLastSelfTriggeredChange.load() < std::chrono::seconds(1)) return;
    if (now - mLastListChangeHandled.load()   < std::chrono::seconds(1)) return;
    // Non-blocking: if a swap/recovery holds the gate, skip this event rather
    // than stall the message thread. The gate also serialises the mDeviceManager
    // reads below — including the source-vs-current comparison — against
    // recovery's recreateDeviceManager() reset(); that comparison used to run
    // ungated at the top of this function.
    std::unique_lock<std::recursive_mutex> gate;
    if (!tryAcquireSwapGate(gate, 1, 0)) {
        DEV_LOG("[hotplug] changeListenerCallback skipped — swap in progress\n");
        return;
    }
    if (source != mDeviceManager.get()) return;

    mLastListChangeHandled = std::chrono::steady_clock::now();

    // MIDI hot-swap is owned by the MIDI subsystem's own native device-change
    // watcher (WinRT DeviceWatcher / CoreMIDI notify / ALSA announce — see
    // rust/supersonic-midi/src/watcher.rs), not JUCE. This audio callback no
    // longer pokes MIDI: a pure-MIDI hot-plug never reached here reliably (it
    // doesn't change the audio device list, and the audio debounce below could
    // swallow it), which is exactly what the dedicated watcher fixes.

    // Collected hot-plug work to schedule after the gate is released.
    std::string pendingSwitchOutput;
    std::string pendingSwitchInput;
    bool schedulePreferredReattach = false;
    bool scheduleInputReattach = false;

    {
        auto devices = listDevices(true);
        auto* dev = mDeviceManager->getCurrentAudioDevice();

        // Active channel counts track the current device's live mask, not the
        // device list, so refresh them on every callback — independent of the
        // device-list fingerprint below. (System mode only: a reinit here would
        // destroy our aggregate device; handleSystemDefaultOutputChanged owns
        // actual default-output changes.)
        if (mDeviceMode.empty() && dev) {
            mCurrentConfig.numOutputChannels =
                dev->getActiveOutputChannels().countNumberOfSetBits();
            mCurrentConfig.numInputChannels =
                dev->getActiveInputChannels().countNumberOfSetBits();
        }

        // Fingerprint the audio device LIST; skip the hotplug decision + device
        // re-report when it's unchanged (e.g. a MIDI-only change that fired this
        // callback) so a MIDI hotplug doesn't churn the GUI's audio list. The
        // field/record separators are control chars that can't appear in a name.
        std::string fingerprint;
        for (auto& d : devices) {
            fingerprint += d.name;
            fingerprint += '\x1f' + std::to_string(d.maxOutputChannels);
            fingerprint += '\x1f' + std::to_string(d.maxInputChannels);
            fingerprint += '\x1f' + std::to_string(d.transportType) + '\x1e';
        }

        if (fingerprint != mLastAudioDeviceFingerprint) {
            mLastAudioDeviceFingerprint = fingerprint;

            std::string currentOutput = mRealOutputDeviceName.empty()
                ? (dev ? dev->getName().toStdString() : "")
                : mRealOutputDeviceName;
            int currentActiveIn = dev ? dev->getActiveInputChannels().countNumberOfSetBits() : 0;

            std::vector<std::string> visibleNames;
            visibleNames.reserve(devices.size());
            for (auto& d : devices) visibleNames.push_back(d.name);

            auto decision = sonicpi::device::decideHotplugAction(
                mPreferredOutputDevice, mPreferredInputDevice,
                currentOutput, currentActiveIn, visibleNames);

            schedulePreferredReattach = decision.switchOutput;
            scheduleInputReattach     = decision.switchInput;
            pendingSwitchOutput       = decision.outputName;
            pendingSwitchInput        = decision.inputName;

            printDeviceList();
        }
    }

    // Release the gate before scheduling — the re-attach runs on the
    // device task lane (NOT the message thread: every other swap
    // deliberately stays off it, and the lane serialises against any
    // mutation already queued) and takes the gate itself.
    gate.unlock();
    if (schedulePreferredReattach) {
        std::string outName = pendingSwitchOutput;
        std::string inName  = pendingSwitchInput;
        fprintf(stderr, "[hotplug] preferred output '%s' returned — scheduling switch "
                "(preferred input='%s')\n", outName.c_str(), inName.c_str());
        fflush(stderr);
        postDeviceTask([this, outName, inName]() {
            switchDevice(outName, 0, 0, false, inName, SwapOrigin::Internal);
        });
    } else if (scheduleInputReattach) {
        std::string inName = pendingSwitchInput;
        fprintf(stderr, "[hotplug] preferred input '%s' returned — scheduling input re-attach\n",
                inName.c_str());
        fflush(stderr);
        postDeviceTask([this, inName]() {
            switchDevice("", 0, 0, false, inName, SwapOrigin::Internal);
        });
    }

    mLastListChangeHandled = std::chrono::steady_clock::now();
}

#ifdef __APPLE__
OSStatus SupersonicEngine::defaultDevicePropertyListenerProc(
    AudioObjectID, UInt32, const AudioObjectPropertyAddress*, void* inClientData)
{
    fprintf(stderr, "[default-output-listener] fired\n"); fflush(stderr);
    auto* self = static_cast<SupersonicEngine*>(inClientData);
    // Straight to the device lane — not the message thread. The lane can
    // afford the bounded gate wait the handler needs; on the MM the
    // handler could only try_lock once and DROPPED the event whenever a
    // reader briefly held the gate, which is why following the macOS
    // default was unreliable.
    self->postDeviceTask([self]() {
        fprintf(stderr, "[default-output-listener] dispatched to handler\n"); fflush(stderr);
        self->handleSystemDefaultOutputChanged();
    });
    return noErr;
}

bool SupersonicEngine::waitForDeviceVisible(const std::string& name, int timeoutMs) {
    if (name.empty() || !mDeviceManager) return false;
    auto* dt = mDeviceManager->getCurrentDeviceTypeObject();
    if (!dt) return false;
    // A freshly-created CoreAudio aggregate isn't in JUCE's list until it
    // rescans, and that can take longer than any fixed sleep. Poll until it
    // shows up so we never open it before JUCE can find it ("No such device").
    constexpr int kStepMs = 50;
    for (int waited = 0; ; waited += kStepMs) {
        dt->scanForDevices();
        std::vector<std::string> names;
        for (auto& n : dt->getDeviceNames(false)) names.push_back(n.toStdString());
        if (sonicpi::device::deviceNameVisible(name, names)) {
            if (waited > 0) {
                fprintf(stderr, "[device-setup] aggregate '%s' visible after %d ms\n",
                        name.c_str(), waited);
                fflush(stderr);
            }
            return true;
        }
        if (waited >= timeoutMs) break;
        juce::Thread::sleep(kStepMs);
    }
    fprintf(stderr, "[device-setup] aggregate '%s' still not visible after %d ms — "
            "aborting open\n", name.c_str(), timeoutMs);
    fflush(stderr);
    return false;
}

void SupersonicEngine::handleSystemDefaultOutputChanged() {
    if (!mDeviceMode.empty()) {
        fprintf(stderr, "[default-output-handler] bail: mDeviceMode='%s' (not empty — not in system mode)\n",
                mDeviceMode.c_str()); fflush(stderr);
        return;
    }
    if (!mRunning.load()) {
        fprintf(stderr, "[default-output-handler] bail: not running\n"); fflush(stderr);
        return;
    }
    auto elapsed = std::chrono::steady_clock::now()
                   - mLastSelfTriggeredChange.load();
    if (elapsed < std::chrono::seconds(2)) {
        fprintf(stderr, "[default-output-handler] bail: %lld ms since last self-triggered change (< 2 s)\n",
                (long long)std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count());
        fflush(stderr);
        return;
    }

    // Read the new macOS system-default output device.
    AudioDeviceID defaultID = kAudioObjectUnknown;
    AudioObjectPropertyAddress addr = {
        kAudioHardwarePropertyDefaultOutputDevice,
        kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain
    };
    UInt32 sz = sizeof(defaultID);
    if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &addr, 0, nullptr, &sz, &defaultID) != noErr
        || defaultID == kAudioObjectUnknown) {
        return;
    }
    CFStringRef nameCF = nullptr;
    UInt32 nameSz = sizeof(nameCF);
    AudioObjectPropertyAddress nameAddr = {
        kAudioDevicePropertyDeviceNameCFString,
        kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain
    };
    if (AudioObjectGetPropertyData(defaultID, &nameAddr, 0, nullptr, &nameSz, &nameCF) != noErr
        || !nameCF) {
        return;
    }
    char buf[256];
    CFStringGetCString(nameCF, buf, sizeof(buf), kCFStringEncodingUTF8);
    CFRelease(nameCF);
    std::string newDefault(buf);

    // If we're on an aggregate, compare the new default against the real
    // (underlying) output we're aggregating, not the aggregate's own name.
    // Non-blocking gate, same as changeListenerCallback: never stall the
    // message thread, and serialise the mDeviceManager /
    // mRealOutputDeviceName reads against recovery's
    // teardownDeviceManager() reset() — these used to run ungated.
    std::string currentOutput;
    {
        // Runs on the device lane: wait for the gate (bounded, same
        // 30x100ms discipline as the debounced switch) rather than
        // dropping the event — a lost default-change never re-fires.
        std::unique_lock<std::recursive_mutex> guard;
        if (!tryAcquireSwapGate(guard, 30, 100)) {
            fprintf(stderr, "[default-output-handler] bail: gate busy for 3s\n");
            fflush(stderr);
            return;
        }
        if (!mDeviceManager) return;
        currentOutput = mRealOutputDeviceName.empty()
            ? (mDeviceManager->getCurrentAudioDevice()
               ? mDeviceManager->getCurrentAudioDevice()->getName().toStdString() : "")
            : mRealOutputDeviceName;
    }

    // Is the new default a virtual device (NDI Audio, Loopback, …)? Read its
    // transport type straight off the device we already resolved.
    uint32_t transport = 0;
    UInt32 tSz = sizeof(transport);
    AudioObjectPropertyAddress tAddr = {
        kAudioDevicePropertyTransportType,
        kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain
    };
    AudioObjectGetPropertyData(defaultID, &tAddr, 0, nullptr, &tSz, &transport);
    const bool newIsVirtual = CoreAudioTransport::isVirtual(transport);

    // Don't chase our own aggregates, no-ops, or virtual devices: chasing a
    // virtual device an app spawned cold-swaps onto something the user never
    // chose and storms the device list.
    if (!sonicpi::device::shouldFollowDefaultOutputChange(
            newDefault, currentOutput, newIsVirtual, sPublishedAppName)) {
        fprintf(stderr, "[device-setup] system default → '%s' (virtual=%d); "
                "not following (staying on '%s')\n",
                newDefault.c_str(), newIsVirtual ? 1 : 0, currentOutput.c_str());
        fflush(stderr);
        return;
    }

    fprintf(stderr, "[device-setup] system default output changed: '%s' -> '%s'\n",
            currentOutput.c_str(), newDefault.c_str());
    fflush(stderr);
    // Route through setDeviceMode("") so we get the wireless/non-wireless
    // branching: non-wireless defaults go via switchDevice (aggregate
    // preserved); wireless defaults go via reinitialiseWithDefaults
    // (JUCE's default-device abstraction, which CoreAudio routes through
    // AirPlay correctly). Already on the device lane — call it directly.
    setDeviceMode("");
}
#endif

std::string SupersonicEngine::setDeviceMode(const std::string& mode) {
    std::string previousMode = mDeviceMode;

    if (mode == "system" || mode.empty()) {
        mDeviceMode.clear();
        // Entering system mode means "follow macOS default" — user has
        // opted out of sticking to a specific hardware device, so drop
        // the hot-plug preference that would otherwise pull them back.
        mPreferredOutputDevice.clear();
    } else {
        mDeviceMode = mode;
        mPreferredOutputDevice = mode;
    }

    if (!mRunning.load()) return "";

    if (mDeviceMode.empty()) {
        // System mode — switch to the current macOS default output while
        // keeping the input device (mic) so live_audio follows the output.
        if (mDeviceManager) {
            fprintf(stderr, "[device-setup] switching to system default\n");
#ifdef __APPLE__
            AudioDeviceID defaultID = kAudioObjectUnknown;
            AudioObjectPropertyAddress addr = {
                kAudioHardwarePropertyDefaultOutputDevice,
                kAudioObjectPropertyScopeGlobal,
                kAudioObjectPropertyElementMain
            };
            UInt32 sz = sizeof(defaultID);
            std::string newDefault;
            if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &addr, 0, nullptr, &sz, &defaultID) == noErr
                && defaultID != kAudioObjectUnknown) {
                CFStringRef nameCF = nullptr;
                UInt32 nameSz = sizeof(nameCF);
                AudioObjectPropertyAddress nameAddr = {
                    kAudioDevicePropertyDeviceNameCFString,
                    kAudioObjectPropertyScopeGlobal,
                    kAudioObjectPropertyElementMain
                };
                if (AudioObjectGetPropertyData(defaultID, &nameAddr, 0, nullptr, &nameSz, &nameCF) == noErr && nameCF) {
                    char buf[256];
                    CFStringGetCString(nameCF, buf, sizeof(buf), kCFStringEncodingUTF8);
                    CFRelease(nameCF);
                    newDefault = buf;
                }
            }
            if (!newDefault.empty()) {
                // Branch based on the new default's transport type:
                //
                //  * Wireless (AirPlay/Bluetooth): go through
                //    reinitialiseWithDefaultsPreservingConfig. JUCE uses
                //    CoreAudio's default-output abstraction, which
                //    correctly routes through AirPlay. Opening AirPlay
                //    by explicit device name (via switchDevice) has
                //    never been reliable in our tests.
                //  * Non-wireless: go through switchDevice so we
                //    preserve/restore the mic via aggregate.
                AudioObjectPropertyAddress tAddr = {
                    kAudioDevicePropertyTransportType,
                    kAudioObjectPropertyScopeGlobal,
                    kAudioObjectPropertyElementMain
                };
                UInt32 tType = 0, tSize = sizeof(tType);
                bool newIsWireless = false;
                if (AudioObjectGetPropertyData(defaultID, &tAddr, 0, nullptr, &tSize, &tType) == noErr) {
                    newIsWireless = CoreAudioTransport::isWireless(tType);
                }
                if (!newIsWireless) {
                    std::string inputName = mRealInputDeviceName;
                    if (inputName.empty() && mCurrentConfig.numInputChannels > 0) {
                        auto setup = mDeviceManager->getAudioDeviceSetup();
                        inputName = setup.inputDeviceName.toStdString();
                    }
                    auto result = switchDevice(newDefault, 0, 0, false, inputName);
                    if (!result.success) return result.error;
                    return {};
                }
                // Wireless default — fall through to the reinitialise
                // path below.
                fprintf(stderr, "[device-setup] system default '%s' is wireless; "
                        "using JUCE default-device init\n", newDefault.c_str());
                fflush(stderr);
            }
#endif
            // Serialise against in-flight swaps. Without the gate this
            // reinit runs concurrently with a debounced switchDevice (which
            // holds mSwapMutex on its worker thread) — two threads driving
            // JUCE's AudioDeviceManager teardown/reopen at once, which can
            // wedge DirectSound's callback thread in a cursor-poll spin and
            // leave the whole server deaf. Bounded retry mirrors
            // executePendingSwitch (~3 s).
            std::unique_lock<std::recursive_mutex> swapGate;
            if (!tryAcquireSwapGate(swapGate, 30, 100)) {
                fprintf(stderr, "[device-setup] system mode init refused: "
                        "swap already in progress\n");
                return "swap already in progress";
            }

            auto err = reinitialiseWithDefaultsPreservingConfig();
            if (err.isNotEmpty()) {
                fprintf(stderr, "[device-setup] system mode init failed: %s\n",
                        err.toRawUTF8());
                // Never leave the engine with a stopped device and no
                // replacement: restart the audio source (falls back to the
                // headless driver when no device is open) so commands keep
                // draining — silent audio beats a deaf server.
                if (mActiveSource.load() != AudioSource::None) {
                    stopAudioSource();
                    startAudioSource();
                }
                return err.toStdString();
            }

            std::string newDevName;
            double newRate = 0.0;
            if (auto* dev = mDeviceManager->getCurrentAudioDevice()) {
                mCurrentConfig.numOutputChannels =
                    dev->getActiveOutputChannels().countNumberOfSetBits();
                mCurrentConfig.numInputChannels =
                    dev->getActiveInputChannels().countNumberOfSetBits();
                newRate = dev->getCurrentSampleRate();
                newDevName = dev->getName().toStdString();
            }

            // Release before the cold swap below — switchDevice acquires the
            // gate itself (try_lock) and would otherwise self-deadlock.
            swapGate.unlock();

            if (newRate > 0 && static_cast<int>(newRate) != mCurrentConfig.sampleRate) {
                fprintf(stderr,
                        "[device-setup] system default has different rate "
                        "(%d -> %.0f Hz) — performing cold swap\n",
                        mCurrentConfig.sampleRate, newRate);
                // Force cold even though JUCE is already at newRate (we
                // just opened it via reinitialiseWithDefaultsPreservingConfig).
                // Without forceCold, switchDevice sees currentRate ==
                // sampleRate and skips the World rebuild, leaving scsynth
                // running at the old rate while JUCE delivers samples at
                // the new rate — mismatch, pitched-down audio.
                switchDevice(newDevName, newRate, 0, /*forceCold=*/true);
            } else {
                printDeviceList();
            }
        }
    } else {
        // Manual mode: switch to the named device.
        // setDeviceMode is called from the Sonic Pi GUI which cannot handle
        // cold swaps (world rebuild destroys the audio graph).  Pre-check
        // whether the target device supports the current sample rate and
        // reject if not — callers that can tolerate cold swaps should use
        // switchDevice() directly (via /supersonic/devices/switch).
        if (mDeviceManager) {
            auto* curDev = mDeviceManager->getCurrentAudioDevice();
            double curRate = curDev ? curDev->getCurrentSampleRate() : 0.0;
            if (curRate > 0) {
                auto rates = probeDeviceSampleRates(mDeviceMode, false);
                bool rateOk = false;
                for (auto r : rates)
                    if (static_cast<int>(r) == static_cast<int>(curRate))
                        rateOk = true;
                if (!rateOk) {
                    fprintf(stderr,
                        "[device-setup] rejecting mode switch to %s: "
                        "current rate %.0f not supported — restart required\n",
                        mDeviceMode.c_str(), curRate);
                    mDeviceMode = previousMode;
                    printDeviceList();
                    return "device requires different sample rate — restart required";
                }
            }
        }

        auto result = switchDevice(mDeviceMode);
        if (!result.success) {
            mDeviceMode = previousMode;  // revert mode on failure
            printDeviceList();
            return result.error;
        }
    }

    printDeviceList();
    return "";
}

void SupersonicEngine::printDeviceList() {
    if (!mDeviceManager) return;

    // Skip rescan — calling scanForDevices() right after a device switch
    // can close the just-opened CoreAudio device. The switch path already
    // rescanned when needed. This path only reports state.
    auto devices = listDevices(false);
    auto current = currentDevice();

    fprintf(stderr, "[audio-devices-start]\n");
    for (auto& dev : devices) {
        char tt[5] = {};
        if (dev.transportType) {
            tt[0] = (char)((dev.transportType >> 24) & 0xFF);
            tt[1] = (char)((dev.transportType >> 16) & 0xFF);
            tt[2] = (char)((dev.transportType >> 8) & 0xFF);
            tt[3] = (char)(dev.transportType & 0xFF);
        }
        fprintf(stderr, "[audio-device-entry] %s|%s|%d|%d|%s\n",
                dev.name.c_str(), dev.typeName.c_str(),
                dev.maxOutputChannels, dev.maxInputChannels,
                dev.transportType ? tt : "?");
    }
    fprintf(stderr, "[audio-device-current] %s|%s|%.0f|%d|%d|%d\n",
            current.name.c_str(), current.typeName.c_str(),
            current.activeSampleRate, current.activeBufferSize,
            current.activeOutputChannels, current.activeInputChannels);
    fprintf(stderr, "[audio-device-mode] %s\n",
            mDeviceMode.empty() ? "system" : mDeviceMode.c_str());
    fprintf(stderr, "[audio-devices-end]\n");
    fflush(stderr);

    // Also push device info via OSC to registered GUI listener
    sendDeviceReport();
}

// --- Recording ---

SupersonicEngine::RecordResult SupersonicEngine::startRecording(
    const std::string& path, const std::string& format, int bitDepth) {
    RecordResult result;
    result.path = path;

    if (isRecording()) {
        result.error = "already recording";
        return result;
    }

    auto* dev = mDeviceManager ? mDeviceManager->getCurrentAudioDevice() : nullptr;
    double sampleRate = dev ? dev->getCurrentSampleRate()
                            : static_cast<double>(mCurrentConfig.sampleRate);
    // Record only the stereo mix (outputs 0/1), not every device channel.
    // Clamped to the device count so a mono device can't over-read outputChannelData.
    int deviceOut = dev ? dev->getActiveOutputChannels().countNumberOfSetBits()
                        : mCurrentConfig.numOutputChannels;
    int numChannels = deviceOut > 2 ? 2 : deviceOut;

    juce::File file{juce::String(path)};
    file.getParentDirectory().createDirectory();

    if (!mRecordThread.isThreadRunning())
        mRecordThread.startThread();

    auto writer = std::make_unique<RecordWriter>(
        path, format, bitDepth, sampleRate, numChannels,
        mRecordThread, static_cast<int>(sampleRate) * 10);

    if (!writer->openedOk()) {
        result.error = "unsupported format/bitDepth (or unwritable path): "
                       + format + "/" + std::to_string(bitDepth);
        return result;
    }

    mAudioCallback.mRecordWriter.store(writer.release(), std::memory_order_release);
    mRecordPath = path;

    result.success = true;
    fprintf(stderr, "[recording] started: %s (%s, %dbit, %.0fHz, %dch)\n",
            path.c_str(), format.c_str(), bitDepth, sampleRate, numChannels);
    return result;
}

SupersonicEngine::RecordResult SupersonicEngine::stopRecording() {
    RecordResult result;
    result.path = mRecordPath;

    if (!isRecording()) {
        result.error = "not recording";
        return result;
    }

    // Pause audio to ensure no in-flight write() calls
    mAudioCallback.pause();
    auto* old = static_cast<RecordWriter*>(
        mAudioCallback.mRecordWriter.exchange(nullptr, std::memory_order_acq_rel));
    mAudioCallback.resume();

    // Delete flushes remaining data and closes the file
    delete old;

    result.success = true;
    fprintf(stderr, "[recording] stopped: %s\n", mRecordPath.c_str());
    mRecordPath.clear();
    return result;
}

bool SupersonicEngine::isRecording() const {
    return mAudioCallback.mRecordWriter.load(std::memory_order_acquire) != nullptr;
}

// --- Purge ---

void SupersonicEngine::purge() {
    // Discard pending IN-ring messages. Callers arrive on two threads — the
    // wake hook pre-tick on the audio thread, cold swap on a control thread
    // while audio is still live — so this only requests; the audio thread,
    // the IN ring's single consumer, applies the flush at the top of its
    // next drain.
    ss_ingress_flush_request();

    // Drop all pending scheduled events
    clear_scheduler();
}

// --- Clock offset ---

void SupersonicEngine::setClockOffset(double offsetSeconds) {
    auto* globalOffset = reinterpret_cast<std::atomic<int32_t>*>(
        sp_arena() + GLOBAL_OFFSET_START);
    globalOffset->store(static_cast<int32_t>(offsetSeconds * 1000.0),
                        std::memory_order_relaxed);
}

double SupersonicEngine::getClockOffset() const {
    auto* globalOffset = reinterpret_cast<const std::atomic<int32_t>*>(
        sp_arena() + GLOBAL_OFFSET_START);
    return globalOffset->load(std::memory_order_relaxed) / 1000.0;
}
