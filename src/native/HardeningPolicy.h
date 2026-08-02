/*
 * HardeningPolicy.h — pure policy helpers that keep the engine robust
 * against hostile audio-graph conditions (see test_hardening_policy.cpp
 * for the field failures each one guards, all from sonic-pi #3553).
 *
 * Header-only and dependency-free so the callback path can use it inline
 * and the test suite can exercise it without a running graph.
 */
#pragma once

#include <algorithm>
#include <string>
#include <vector>

// NB: injecting stream.dont-remix via PIPEWIRE_PROPS is deliberately NOT
// one of these helpers. With JUCE's unlabeled channel positions
// WirePlumber links an un-remixed multichannel stream to nothing at all,
// so it starves and the watchdog yanks the user off their chosen device.
// Multichannel routing is the patchbay device's job.

namespace hardening {

// Gap size beyond which an audio-callback pause is a stall worth logging.
// Floored at 150ms (the point where scheduled client threads overshoot
// their sched-ahead window), but scaled to 2.5 healthy callback periods so
// a graph running us at a huge-but-regular quantum — which delivers one
// callback per period by design — is not misread as stalling.
inline double stallThresholdUs(int numSamples, double sampleRate) {
    constexpr double kFloorUs = 150'000.0;
    if (numSamples <= 0 || sampleRate <= 0.0)
        return kFloorUs;
    const double periodUs = (double) numSamples / sampleRate * 1'000'000.0;
    return std::max(kFloorUs, 2.5 * periodUs);
}

// True when the session manager's *configured* default device names a node
// that is absent from the graph while other candidates exist — the stale
// state left behind by unplugging or swapping a sound card. An empty graph
// is not a ghost: during boot the metadata routinely arrives before the
// node registry has populated.
inline bool defaultNodeMissing(const std::string& configuredName,
                               const std::vector<std::string>& presentNodeNames) {
    if (configuredName.empty() || presentNodeNames.empty())
        return false;
    return std::find(presentNodeNames.begin(), presentNodeNames.end(), configuredName)
           == presentNodeNames.end();
}

} // namespace hardening
