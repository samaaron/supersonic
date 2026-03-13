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
    bool isDefault = false;
};

struct CurrentDeviceInfo : DeviceInfo {
    double activeSampleRate    = 0.0;
    int    activeBufferSize    = 0;
    int    activeOutputChannels = 0;
    int    activeInputChannels  = 0;
    int    outputLatencySamples = 0;
    int    inputLatencySamples  = 0;
};

enum class SwapType { Hot, Cold };

struct SwapResult {
    bool        success = false;
    SwapType    type    = SwapType::Hot;
    std::string error;
    std::string deviceName;
    double      sampleRate  = 0.0;
    int         bufferSize  = 0;
};
