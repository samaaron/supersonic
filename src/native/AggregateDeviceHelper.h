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
// desiredSampleRate: the rate the engine wants to run at (scsynth's
// mBufLength depends on it, as does JUCE's setup.sampleRate that we
// pass to setAudioDeviceSetup afterwards). Passing 0 means "pick the
// input device's current rate" — legacy behaviour, but risks
// aggregate-level SRC distortion when sub-devices end up at a rate
// the engine isn't expecting.
std::string createOrUpdate(const std::string& outputDeviceName,
                           const std::string& inputDeviceName,
                           double desiredSampleRate = 0);

// Destroy the managed Aggregate Device if one exists.
void destroy();

// Destroy the PREVIOUS aggregate (if any) — call this AFTER the caller
// has switched JUCE to the new aggregate via setAudioDeviceSetup.
// Destroying the old aggregate before JUCE releases it causes JUCE's
// AudioComponentInstanceDispose to crash on the dangling CoreAudio ID.
void destroyPrevious();

// Returns true if a managed Aggregate Device currently exists.
bool exists();

// Returns the name of the current managed Aggregate Device, or ""
std::string currentName();

// Clean up any orphaned aggregate from a previous crash.
// Should be called at boot before any audio device initialization.
void cleanupOrphaned();

// Returns true if the current managed aggregate has kernel drift
// compensation active on its input sub-device (sub-devices have
// different clock domains). False for same-clock aggregates (e.g.
// MBP Speakers + MBP Mic — both built-in Apple Silicon audio block)
// where the SRC IOProc is a no-op. Callers use this to decide whether
// to enforce headroom-related limits like the 256-sample buffer floor.
bool driftCompensationEnabled();

} // namespace AggregateDeviceHelper

#endif // __APPLE__
