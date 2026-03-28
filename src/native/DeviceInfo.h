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

    // Returns true if this device is suitable for use as an input device
    // in combination with a different output device. Bluetooth and AirPlay
    // inputs force low-quality codec modes that break audio quality.
    bool isSuitableForInput() const {
        constexpr uint32_t kBluetooth   = 0x626C7565; // 'blue'
        constexpr uint32_t kBluetoothLE = 0x626C6561; // 'blea'
        constexpr uint32_t kAirPlay     = 0x61697270; // 'airp'
        return transportType != kBluetooth
            && transportType != kBluetoothLE
            && transportType != kAirPlay;
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
