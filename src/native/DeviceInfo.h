/*
 * DeviceInfo.h — Transport-neutral POD structs for device management
 */
#pragma once

#include <cstdint>
#include <string>
#include <vector>

// CoreAudio transportType fourCC codes. Useful when dealing with a raw
// transportType from CoreAudio without a full DeviceInfo.
namespace CoreAudioTransport {
    inline constexpr uint32_t kBluetooth     = 0x626C7565; // 'blue'
    inline constexpr uint32_t kBluetoothLE   = 0x626C6561; // 'blea'
    inline constexpr uint32_t kAirPlay       = 0x61697270; // 'airp'
    inline constexpr uint32_t kContinuityCam = 0x63637764; // 'ccwd'
    inline constexpr uint32_t kVirtual       = 0x76697274; // 'virt'

    inline bool isWireless(uint32_t t) {
        return t == kBluetooth || t == kBluetoothLE
            || t == kAirPlay   || t == kContinuityCam;
    }
    inline bool isVirtual(uint32_t t) { return t == kVirtual; }
}

struct DeviceInfo {
    std::string name;
    std::string typeName;                    // "Windows Audio", "ASIO", "CoreAudio", "ALSA"
    std::vector<double> availableSampleRates;
    std::vector<int>    availableBufferSizes;
    int  maxOutputChannels = 0;
    int  maxInputChannels  = 0;
    uint32_t transportType = 0;              // CoreAudio transport type (macOS only)

    // Bluetooth and AirPlay use wireless codecs that are unsuitable for
    // low-latency audio aggregation (HFP 16kHz mono, AirPlay buffering).
    bool isWirelessTransport() const { return CoreAudioTransport::isWireless(transportType); }

    // Virtual devices (Loopback Audio, Blackhole, SoundSource, etc.) don't
    // have a real hardware clock — their sample clock is driven by the OS
    // scheduler. Using a virtual device as an aggregate's master clock
    // crashes CoreAudio's drift-compensation SRC inside AudioUnitRender.
    bool isVirtualTransport() const { return CoreAudioTransport::isVirtual(transportType); }

    // Suitable for input: exclude Bluetooth/AirPlay (force low-quality codecs)
    bool isSuitableForInput() const { return !isWirelessTransport(); }

    // Suitable for aggregation: exclude only wireless (Bluetooth/AirPlay).
    // Virtual devices (Loopback, Blackhole) CAN be aggregated when the
    // master clock is set to the HARDWARE side (matches Ardour / JACK2
    // patterns). Using a virtual device as master fails — see
    // AggregateDeviceHelper::createOrUpdate for master-selection logic.
    bool isSuitableForAggregate() const {
        return !isWirelessTransport();
    }

    // Hide platform-specific clutter from the GUI dropdown list. On macOS
    // the wireless / virtual predicates above cover everything. On Linux
    // JUCE's ALSA backend exposes a raft of redundant PCM nodes (surround
    // 2.1/4.0/5.1/7.1, Direct sample mixing / snooping) that users don't
    // want to scroll through. Additionally, when PipeWire is active
    // (pipewireActive=true), direct-hardware ALSA PCMs cannot be opened
    // — PipeWire holds the card exclusive — so selecting them cascades
    // to "device didn't start" + engine rollback into a broken state.
    // Hide them too. Engine still accepts any name via switchDevice —
    // this only affects the push list used by dropdowns.
    bool isPlatformClutter(bool pipewireActive = false) const {
#ifdef __linux__
        if (typeName != "ALSA") return false;
        // Always hidden — never useful from a Sonic Pi GUI:
        if (name.find("Surround") != std::string::npos) return true;
        if (name.find("Direct sample mixing device") != std::string::npos) return true;
        if (name.find("Direct sample snooping device") != std::string::npos) return true;
        // Hidden only when PipeWire owns the card (the common case on
        // modern Linux desktops): direct-hardware PCMs would fail to
        // open. On pure-ALSA boxes (no PipeWire) these are the main
        // hardware entry points and must stay visible.
        if (pipewireActive) {
            if (name.find("Direct hardware device") != std::string::npos) return true;
            if (name.find("Front output / input") != std::string::npos) return true;
        }
        return false;
#else
        (void)pipewireActive;
        return false;
#endif
    }
};

struct CurrentDeviceInfo : DeviceInfo {
    double activeSampleRate    = 0.0;
    int    activeBufferSize    = 0;
    int    activeOutputChannels = 0;
    int    activeInputChannels  = 0;
    int    outputLatencySamples = 0;
    int    inputLatencySamples  = 0;
    std::string inputDeviceName;
};

enum class SwapType { Hot, Cold };

struct SwapResult {
    bool        success = false;
    SwapType    type    = SwapType::Hot;
    std::string error;
    std::string deviceName;
    std::string inputDeviceName;
    double      sampleRate  = 0.0;
    int         bufferSize  = 0;
    // Set when the caller asked for an input device but it couldn't be
    // opened (e.g. Windows microphone privacy denied). The swap still
    // succeeded for the output, so we don't roll back the whole thing —
    // we just clear the input. The client surface this so the user knows
    // why their mic isn't live.
    bool        inputUnavailable = false;
    std::string inputUnavailableReason;
};
