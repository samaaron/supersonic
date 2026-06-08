/*
 * test_event_scheduler.cpp — the deferred-event scheduler in isolation:
 * events in (enqueue) → stored → out on time (tick emits to the OUT ring only
 * once due, framed [dest][osc]). No engine or hardware needed.
 */
#include <catch2/catch_test_macros.hpp>

#include "scheduler/EventScheduler.h"
#include "src/shared_memory.h"

#include <cstring>

namespace {
// True if the ring has an unread message.
bool ringHasMessage(EventScheduler& es) {
    return es.outHead()->load() != es.outTail()->load();
}
} // namespace

TEST_CASE("EventScheduler holds events until due, then emits framed [dest][osc]",
          "[event_scheduler]") {
    EventScheduler es;
    const uint8_t osc[] = {0x90, 60, 100};   // stand-in inner payload
    REQUIRE(es.enqueue(/*when*/ 1000, EventScheduler::DEST_MIDI, osc, sizeof(osc)));

    // Not due yet → nothing emitted.
    es.tick(/*nextOscTime*/ 500);
    CHECK_FALSE(ringHasMessage(es));

    // Due → one message on the OUT ring.
    es.tick(/*nextOscTime*/ 2000);
    REQUIRE(ringHasMessage(es));

    // Decode the framed message: Message header + [dest:u32][osc].
    const uint8_t* buf = es.outBuffer();
    int32_t tail = es.outTail()->load();
    Message hdr;
    std::memcpy(&hdr, buf + tail, sizeof(hdr));
    CHECK(hdr.magic == 0xDEADBEEF);

    const uint8_t* payload = buf + tail + sizeof(Message);
    uint32_t dest;
    std::memcpy(&dest, payload, sizeof(dest));
    CHECK(dest == static_cast<uint32_t>(EventScheduler::DEST_MIDI));

    const uint32_t oscLen = hdr.length - sizeof(Message) - sizeof(uint32_t);
    REQUIRE(oscLen == sizeof(osc));
    CHECK(std::memcmp(payload + sizeof(uint32_t), osc, sizeof(osc)) == 0);

    // A second tick with nothing pending emits nothing more.
    int32_t head = es.outHead()->load();
    es.tick(3000);
    CHECK(es.outHead()->load() == head);
}

TEST_CASE("EventScheduler emits each due event once", "[event_scheduler]") {
    EventScheduler es;
    const uint8_t a[] = {0xB0, 7, 1};
    const uint8_t b[] = {0xB0, 7, 2};
    es.enqueue(100, EventScheduler::DEST_MIDI, a, sizeof(a));
    es.enqueue(200, EventScheduler::DEST_MIDI, b, sizeof(b));

    es.tick(150);                     // only `a` due
    REQUIRE(ringHasMessage(es));
    int32_t afterFirst = es.outHead()->load();

    es.tick(150);                     // `a` already emitted, `b` not due
    CHECK(es.outHead()->load() == afterFirst);

    es.tick(250);                     // now `b` due
    CHECK(es.outHead()->load() != afterFirst);
}

TEST_CASE("EventScheduler fires in the first tick at/after target — never early",
          "[event_scheduler]") {
    EventScheduler es;
    const uint8_t osc[] = {0xF8};
    const int64_t block  = 1000;     // OSC-time units advanced per audio tick
    const int64_t target = 10'500;   // mid-block target
    es.enqueue(target, EventScheduler::DEST_MIDI, osc, sizeof(osc));

    int64_t t = 0;
    int64_t firedAtNextOsc = -1;
    for (int i = 0; i < 20 && firedAtNextOsc < 0; ++i) {
        const int64_t nextOscTime = t + block;
        const int32_t before = es.outHead()->load();
        es.tick(nextOscTime);
        if (es.outHead()->load() != before) firedAtNextOsc = nextOscTime;
        t = nextOscTime;
    }
    REQUIRE(firedAtNextOsc >= 0);
    CHECK(firedAtNextOsc >= target);            // never early
    CHECK(firedAtNextOsc - target < block);     // within one block (block-granular)
}

TEST_CASE("EventScheduler rejects oversized payloads", "[event_scheduler]") {
    EventScheduler es;
    std::vector<uint8_t> huge(4096, 0);
    CHECK_FALSE(es.enqueue(0, EventScheduler::DEST_MIDI, huge.data(),
                           static_cast<uint32_t>(huge.size())));
    CHECK(es.dropped() == 1);
}
