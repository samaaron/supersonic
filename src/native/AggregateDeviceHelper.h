/*
 * AggregateDeviceHelper.h — macOS CoreAudio Aggregate Device management
 *
 * Creates a single Aggregate Device combining separate input and output
 * devices with kernel-level drift correction. This avoids JUCE's
 * AudioIODeviceCombiner which has no drift correction and fails with
 * network/wireless devices (iPhone Continuity mic, AirPods, etc.).
 */
#pragma once

#include <string>

#ifdef __APPLE__

namespace AggregateDeviceHelper {

// Create or update the managed Aggregate Device combining the given
// output and input devices. Returns the Aggregate Device name to use
// with JUCE's AudioDeviceManager. The output device is used as the
// clock source; drift correction is enabled on the input device.
//
// If outputUID or inputUID is empty, destroys any existing aggregate
// and returns empty string.
//
// The aggregate device is named "SuperSonic" and there is only ever
// one managed instance at a time.
std::string createOrUpdate(const std::string& outputDeviceName,
                           const std::string& inputDeviceName);

// Destroy the managed Aggregate Device if one exists.
void destroy();

// Returns true if a managed Aggregate Device currently exists.
bool exists();

// Returns the name of the current managed Aggregate Device, or ""
std::string currentName();

} // namespace AggregateDeviceHelper

#endif // __APPLE__
