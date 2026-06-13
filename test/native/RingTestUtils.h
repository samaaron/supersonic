/*
 * RingTestUtils.h — helpers for asserting against the EventScheduler OUT ring.
 *
 * The deferred-event scheduler frames each emitted event as [Message hdr]
 * [dest:u32][osc]. Tests walk that ring to count what the engine scheduled.
 * Shared so the frame layout is decoded in exactly one place.
 */
#pragma once

#include "scheduler/EventScheduler.h"
#include "src/shared_memory.h"

#include <cstdint>
#include <cstring>

namespace ring_test {

// Walk the OUT ring from `fromHead` to `toHead`, counting framed
// [dest:u32][osc] DEST_MIDI messages whose OSC address equals `addr`. The MIDI
// dispatch thread may consume concurrently, but consumption only moves the
// tail — bytes between two head snapshots stay valid until the writer laps the
// ring, which a handful of clock ticks cannot cause.
inline int countOutRingByAddr(EventScheduler& es, int32_t fromHead, int32_t toHead, const char* addr) {
    const uint8_t* buf  = es.outBuffer();
    const uint32_t size = es.outSize();
    auto at = [&](uint32_t p) { return buf[p % size]; };
    int count = 0;
    uint32_t pos = static_cast<uint32_t>(fromHead);
    while (pos != static_cast<uint32_t>(toHead)) {
        Message hdr{};
        for (uint32_t i = 0; i < sizeof(Message); ++i) reinterpret_cast<uint8_t*>(&hdr)[i] = at(pos + i);
        if (hdr.magic != 0xDEADBEEFu || hdr.length < sizeof(Message)) break;
        const uint32_t dataPos = pos + static_cast<uint32_t>(sizeof(Message));
        uint32_t dest = 0;
        for (uint32_t i = 0; i < sizeof(dest); ++i) reinterpret_cast<uint8_t*>(&dest)[i] = at(dataPos + i);
        char got[40] = {0};
        for (uint32_t i = 0; i < sizeof(got) - 1; ++i) {
            const char ch = static_cast<char>(at(dataPos + sizeof(dest) + i));
            got[i] = ch;
            if (ch == '\0') break;
        }
        if (dest == static_cast<uint32_t>(EventScheduler::DEST_MIDI) && std::strcmp(got, addr) == 0)
            ++count;
        pos = (pos + hdr.length) % size;
    }
    return count;
}

} // namespace ring_test
