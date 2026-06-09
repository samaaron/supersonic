/*
 * timeline_osc.h — shared OSC serialization for the /clock/timelines listing.
 *
 * The flat row layout [name raw bpm(f) clocking(i) stale(i) primary(i)]* is
 * emitted from two places (the /clock/timelines/get reply in EngineClock and the
 * /clock/timelines push in MidiControl); this keeps the wire shape in one spot.
 * Templated on the stream type so the header carries no oscpack dependency.
 */
#pragma once

#include "SuperClock.h"

#include <cstdint>
#include <vector>

template <typename Stream>
inline void appendTimelineRows(Stream& s, const std::vector<SuperClock::TimelineInfo>& tls) {
    for (const auto& t : tls)
        s << t.name.c_str()
          << t.raw.c_str()
          << static_cast<float>(t.bpm)
          << static_cast<std::int32_t>(t.clocking ? 1 : 0)
          << static_cast<std::int32_t>(t.stale ? 1 : 0)
          << static_cast<std::int32_t>(t.primary ? 1 : 0);
}
