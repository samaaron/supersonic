/*
 * FakeAudioDevice.h — scriptable JUCE audio devices for hermetic engine tests.
 *
 * The seam is Config::deviceManagerFactory: the engine constructs its
 * juce::AudioDeviceManager through it (boot AND recovery recreate), so a
 * test can hand the engine a REAL manager whose only device types are
 * fakes. Every line of manager logic — name validation, setup resolution,
 * type switching, change broadcasts — is JUCE's own; only the device edge
 * (open/start/callback) is simulated. That is deliberately the opposite
 * of mocking the manager: the manager's behaviour is part of what the
 * engine tests must exercise.
 *
 * A FakeAudioIODevice delivers real callbacks from its own thread at
 * roughly hardware cadence, so waitForBlocks / the watchdog / first-tick
 * barriers behave as they do against hardware.
 *
 * Hermeticity caveat (macOS): init()'s default-device pre-checks and the
 * aggregate-promotion block still call real CoreAudio when
 * cfg.numInputChannels != 0 and no -H device is given. Until the boot
 * path is restructured (P4), hermetic tests boot with
 * cfg.hardwareDevice = "<fake name>" and cfg.numInputChannels = 0, which
 * skips both. fakeEngineConfig() below encodes that recipe.
 */
#pragma once

#include "SupersonicEngine.h"
#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace fake_audio {

// One simulated hardware device. Tests mutate these (through the shared
// FakeSystem) to script failures; flags are read at open/start time.
struct FakeDeviceSpec {
    std::string name;
    int maxOutputChannels = 2;
    int maxInputChannels  = 2;
    std::vector<double> sampleRates { 44100.0, 48000.0 };
    std::vector<int>    bufferSizes { 64, 128, 256, 512 };

    bool failOpen      = false;  // open() errors outright
    bool failInputOpen = false;  // open() errors iff input channels requested
    bool failStart     = false;  // open() succeeds but callbacks never tick
};

// The simulated machine: device types and their devices, shared between
// the test (which scripts it) and every manager the factory builds —
// including managers built later by recovery's recreateDeviceManager.
struct FakeSystem {
    struct TypeSpec {
        std::string typeName;
        std::vector<std::shared_ptr<FakeDeviceSpec>> devices;
        int defaultDeviceIndex = 0;
    };
    std::vector<TypeSpec> types;

    std::shared_ptr<FakeDeviceSpec> device(const std::string& name) {
        for (auto& t : types)
            for (auto& d : t.devices)
                if (d->name == name) return d;
        return nullptr;
    }
};

class FakeAudioIODevice : public juce::AudioIODevice {
public:
    FakeAudioIODevice(std::shared_ptr<FakeDeviceSpec> outSpec,
                      std::shared_ptr<FakeDeviceSpec> inSpec,
                      const juce::String& typeName)
        : juce::AudioIODevice(outSpec ? juce::String(outSpec->name)
                                      : juce::String(inSpec->name),
                              typeName),
          mOut(std::move(outSpec)), mIn(std::move(inSpec)) {}

    ~FakeAudioIODevice() override { close(); }

    juce::StringArray getOutputChannelNames() override {
        return channelNames(mOut ? mOut->maxOutputChannels : 0, "Out");
    }
    juce::StringArray getInputChannelNames() override {
        return channelNames(mIn ? mIn->maxInputChannels : 0, "In");
    }
    juce::Array<double> getAvailableSampleRates() override {
        juce::Array<double> r;
        for (double sr : primary()->sampleRates) r.add(sr);
        return r;
    }
    juce::Array<int> getAvailableBufferSizes() override {
        juce::Array<int> r;
        for (int b : primary()->bufferSizes) r.add(b);
        return r;
    }
    int getDefaultBufferSize() override { return 128; }

    juce::String open(const juce::BigInteger& inputChannels,
                      const juce::BigInteger& outputChannels,
                      double sampleRate, int bufferSizeSamples) override {
        // Error strings mimic real JUCE drivers, which name the device.
        // Wording is NOT load-bearing: the engine attributes input-side
        // failures by retrying output-only, not by parsing these.
        if (primary()->failOpen)
            return "Failed to open device: " + juce::String(primary()->name);
        if (mIn && mIn->failInputOpen && inputChannels.countNumberOfSetBits() > 0)
            return "Failed to open input device: " + juce::String(mIn->name);

        // Clamp requested bits to capacity — CoreAudio semantics.
        mActiveOut = outputChannels;
        mActiveOut.setRange(mOut ? mOut->maxOutputChannels : 0, 256, false);
        mActiveIn = inputChannels;
        mActiveIn.setRange(mIn ? mIn->maxInputChannels : 0, 256, false);

        mRate = pickNearest(primary()->sampleRates, sampleRate);
        mBufferSize = pickNearest(primary()->bufferSizes,
                                  bufferSizeSamples > 0 ? bufferSizeSamples
                                                        : getDefaultBufferSize());
        mOpen = true;
        return {};
    }

    void close() override { stop(); mOpen = false; }
    bool isOpen() override { return mOpen; }

    void start(juce::AudioIODeviceCallback* callback) override {
        if (!mOpen || mPlaying.load()) return;
        mCallback = callback;
        if (callback) callback->audioDeviceAboutToStart(this);
        mPlaying.store(true);
        if (!primary()->failStart)
            mThread = std::thread([this] { tickLoop(); });
    }

    void stop() override {
        if (!mPlaying.exchange(false)) return;
        if (mThread.joinable()) mThread.join();
        if (mCallback) mCallback->audioDeviceStopped();
        mCallback = nullptr;
    }

    bool isPlaying() override { return mPlaying.load(); }
    juce::String getLastError() override { return {}; }
    int getCurrentBufferSizeSamples() override { return mBufferSize; }
    double getCurrentSampleRate() override { return mRate; }
    int getCurrentBitDepth() override { return 32; }
    juce::BigInteger getActiveOutputChannels() const override { return mActiveOut; }
    juce::BigInteger getActiveInputChannels() const override { return mActiveIn; }
    int getOutputLatencyInSamples() override { return 0; }
    int getInputLatencyInSamples() override { return 0; }

private:
    // The spec that answers rate/buffer/failure questions: the output side
    // when present (it is the clock master everywhere in the engine), else
    // the input side.
    const std::shared_ptr<FakeDeviceSpec>& primary() const {
        return mOut ? mOut : mIn;
    }

    static juce::StringArray channelNames(int count, const char* stem) {
        juce::StringArray names;
        for (int i = 0; i < count; ++i)
            names.add(juce::String(stem) + " " + juce::String(i + 1));
        return names;
    }

    static double pickNearest(const std::vector<double>& xs, double want) {
        double best = xs.empty() ? want : xs.front();
        for (double x : xs)
            if (std::abs(x - want) < std::abs(best - want)) best = x;
        return best;
    }
    static int pickNearest(const std::vector<int>& xs, int want) {
        int best = xs.empty() ? want : xs.front();
        for (int x : xs)
            if (std::abs(x - want) < std::abs(best - want)) best = x;
        return best;
    }

    void tickLoop() {
        const int nOut = mActiveOut.countNumberOfSetBits();
        const int nIn  = mActiveIn.countNumberOfSetBits();
        std::vector<std::vector<float>> outBufs(
            static_cast<size_t>(std::max(nOut, 1)),
            std::vector<float>(static_cast<size_t>(mBufferSize), 0.0f));
        std::vector<std::vector<float>> inBufs(
            static_cast<size_t>(std::max(nIn, 1)),
            std::vector<float>(static_cast<size_t>(mBufferSize), 0.0f));
        std::vector<float*>       outPtrs;
        std::vector<const float*> inPtrs;
        for (auto& b : outBufs) outPtrs.push_back(b.data());
        for (auto& b : inBufs)  inPtrs.push_back(b.data());

        const auto period = std::chrono::microseconds(
            static_cast<int64_t>(1e6 * mBufferSize / mRate));
        while (mPlaying.load()) {
            if (mCallback)
                mCallback->audioDeviceIOCallbackWithContext(
                    nIn > 0 ? inPtrs.data() : nullptr, nIn,
                    outPtrs.data(), nOut, mBufferSize, {});
            std::this_thread::sleep_for(period);
        }
    }

    std::shared_ptr<FakeDeviceSpec> mOut, mIn;
    juce::AudioIODeviceCallback* mCallback = nullptr;
    juce::BigInteger mActiveOut, mActiveIn;
    std::thread mThread;
    std::atomic<bool> mPlaying { false };
    bool mOpen = false;
    double mRate = 48000.0;
    int mBufferSize = 128;
};

class FakeAudioIODeviceType : public juce::AudioIODeviceType {
public:
    FakeAudioIODeviceType(std::shared_ptr<FakeSystem> system, size_t typeIndex)
        : juce::AudioIODeviceType(
              juce::String(system->types[typeIndex].typeName)),
          mSystem(std::move(system)), mTypeIndex(typeIndex) {}

    void scanForDevices() override { mScanned = true; }

    juce::StringArray getDeviceNames(bool wantInputNames) const override {
        juce::StringArray names;
        for (auto& d : spec().devices) {
            if (wantInputNames ? d->maxInputChannels > 0
                               : d->maxOutputChannels > 0)
                names.add(juce::String(d->name));
        }
        return names;
    }

    int getDefaultDeviceIndex(bool) const override {
        return spec().defaultDeviceIndex;
    }

    int getIndexOfDevice(juce::AudioIODevice* device, bool asInput) const override {
        if (!device) return -1;
        return getDeviceNames(asInput).indexOf(device->getName());
    }

    bool hasSeparateInputsAndOutputs() const override { return true; }

    juce::AudioIODevice* createDevice(const juce::String& outputDeviceName,
                                      const juce::String& inputDeviceName) override {
        auto out = find(outputDeviceName.toStdString());
        auto in  = find(inputDeviceName.toStdString());
        if (!out && !in) return nullptr;
        return new FakeAudioIODevice(out, in, getTypeName());
    }

private:
    const FakeSystem::TypeSpec& spec() const {
        return mSystem->types[mTypeIndex];
    }
    std::shared_ptr<FakeDeviceSpec> find(const std::string& name) const {
        if (name.empty()) return nullptr;
        for (auto& d : spec().devices)
            if (d->name == name) return d;
        return nullptr;
    }

    std::shared_ptr<FakeSystem> mSystem;
    size_t mTypeIndex;
    bool mScanned = false;
};

class FakeDeviceManager : public juce::AudioDeviceManager {
public:
    explicit FakeDeviceManager(std::shared_ptr<FakeSystem> system)
        : mSystem(std::move(system)) {}

    void createAudioDeviceTypes(
        juce::OwnedArray<juce::AudioIODeviceType>& types) override {
        for (size_t i = 0; i < mSystem->types.size(); ++i)
            types.add(new FakeAudioIODeviceType(mSystem, i));
    }

private:
    std::shared_ptr<FakeSystem> mSystem;
};

// A one-output-one-input machine on a single driver — the common case.
inline std::shared_ptr<FakeSystem> makeSimpleSystem() {
    auto sys = std::make_shared<FakeSystem>();
    auto out = std::make_shared<FakeDeviceSpec>();
    out->name = "Fake Speakers";
    out->maxInputChannels = 0;
    auto duplex = std::make_shared<FakeDeviceSpec>();
    duplex->name = "Fake Interface";
    duplex->maxOutputChannels = 2;
    duplex->maxInputChannels  = 2;
    auto mic = std::make_shared<FakeDeviceSpec>();
    mic->name = "Fake Microphone";
    mic->maxOutputChannels = 0;
    sys->types.push_back({ "FakeDriver", { out, duplex, mic }, 0 });
    return sys;
}

inline std::function<std::unique_ptr<juce::AudioDeviceManager>()>
makeFactory(std::shared_ptr<FakeSystem> system) {
    return [system] {
        return std::make_unique<FakeDeviceManager>(system);
    };
}

// The hermetic boot recipe (see header comment): a named -H open of a
// fake device with inputs disabled, real device paths active, watchdog
// off unless the test opts in.
inline SupersonicEngine::Config fakeEngineConfig(
        std::shared_ptr<FakeSystem> system,
        const std::string& bootOutput) {
    SupersonicEngine::Config cfg;
    cfg.sampleRate           = 48000;
    cfg.udpPort              = 0;
    cfg.numBuffers           = 1024;
    cfg.maxNodes             = 1024;
    cfg.maxGraphDefs         = 512;
    cfg.maxWireBufs          = 64;
    cfg.headless             = false;
    cfg.numOutputChannels    = 2;
    cfg.numInputChannels     = 0;   // skip mac aggregate-promotion CoreAudio reads
    cfg.hardwareDevice       = bootOutput;  // skip mac default-output CoreAudio reads
    cfg.deviceManagerFactory = makeFactory(std::move(system));
    return cfg;
}

} // namespace fake_audio
