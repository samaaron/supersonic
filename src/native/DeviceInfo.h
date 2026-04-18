/*
 * DeviceInfo.h — Transport-neutral POD structs for device management
 */
#pragma once

#include <string>
#include <vector>

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
    bool isWirelessTransport() const {
        constexpr uint32_t kBluetooth      = 0x626C7565; // 'blue'
        constexpr uint32_t kBluetoothLE    = 0x626C6561; // 'blea'
        constexpr uint32_t kAirPlay        = 0x61697270; // 'airp'
        constexpr uint32_t kContinuityCam  = 0x63637764; // 'ccwd'
        return transportType == kBluetooth
            || transportType == kBluetoothLE
            || transportType == kAirPlay
            || transportType == kContinuityCam;
    }

    // Virtual devices (Loopback Audio, Blackhole, SoundSource, etc.) don't
    // have a real hardware clock — their sample clock is driven by the OS
    // scheduler. Aggregating a virtual device with real hardware results in
    // severe clock drift that macOS can't compensate, causing the aggregate
    // to fail within a buffer or two.
    bool isVirtualTransport() const {
        constexpr uint32_t kVirtual = 0x76697274; // 'virt'
        return transportType == kVirtual;
    }

    // Suitable for input: exclude Bluetooth/AirPlay (force low-quality codecs)
    bool isSuitableForInput() const { return !isWirelessTransport(); }

    // Suitable for aggregation: exclude wireless (Bluetooth/AirPlay).
    // Virtual devices (Loopback, Blackhole) CAN be aggregated but may
    // introduce drift — SuperSonic enables drift compensation to mitigate.
    bool isSuitableForAggregate() const {
        return !isWirelessTransport();
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
};
