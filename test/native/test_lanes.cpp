/*
 * test_lanes.cpp — contract tests for the lanes C ABI (src/lanes/lanes.h) and
 * the shared ring walker (src/lanes/ring_drain.h).
 *
 * Exercises the ABI surface directly — ingress write, both egress drains, the
 * NRT egress producer, and input validation — against a test-local arena (the
 * engine globals are saved/restored so the rest of the suite is untouched).
 * The walker tests pin the consumer semantics every C++ reader relies on:
 * in-place delivery, consume-after-callback, Retain backpressure, scratch
 * linearisation of wrapped payloads, cursor repair, and padding follow.
 * The full ingress→tick→egress path is already covered by every engine test,
 * since native ingress IS ss_ingress_write and the per-block tick IS ss_tick.
 */
#include <catch2/catch_test_macros.hpp>

#include "lanes/lanes.h"                // the host ABI under test (self-guards extern "C")
#include "lanes/lanes_internal.h"       // ss_egress_nrt_write — inject NRT frames for the drain test
#include "audio_processor.h"            // shared_memory, control, memory_initialized
#include "shared_memory.h"             // arena layout + EgressRoute
#include "lanes/ring_drain.h"          // ss_drain_ring — the walker under test
#include "workers/RingBufferWriter.h"  // inject frames for walker/drain tests

#include <atomic>
#include <cstring>
#include <string>
#include <vector>

// The audio-thread OUT-ring writer (audio_processor.cpp, global namespace) —
// exercised here to pin it to the same wire convention the walker reads.
extern bool ring_buffer_write(
    uint8_t* buffer_start,
    uint32_t buffer_size,
    std::atomic<int32_t>* head,
    std::atomic<int32_t>* tail,
    std::atomic<int32_t>* sequence,
    uint32_t route,
    uint32_t source_id,
    const void* data,
    uint32_t data_size,
    std::atomic<uint32_t>* status_flags,
    PerformanceMetrics* metrics);

namespace {

// Point the engine globals at a zeroed, test-local arena for the duration of a
// test, then restore them so no other test in the binary is affected. metrics
// must be redirected too: the RT egress drain counts into it.
struct LanesArena {
    std::vector<uint8_t> buf;
    uint8_t*             savedMem;
    ControlPointers*     savedCtrl;
    PerformanceMetrics*  savedMetrics;
    bool                 savedInit;

    LanesArena() : buf(TOTAL_BUFFER_SIZE, 0) {
        savedMem  = shared_memory; savedCtrl = control;
        savedMetrics = metrics;    savedInit = memory_initialized;
        shared_memory      = buf.data();
        control            = reinterpret_cast<ControlPointers*>(buf.data() + CONTROL_START);
        metrics            = reinterpret_cast<PerformanceMetrics*>(buf.data() + METRICS_START);
        memory_initialized = true;
        ss_lanes_reset_drains();  // fresh arena → fresh sequence-gap tracking
    }
    ~LanesArena() {
        shared_memory = savedMem; control = savedCtrl;
        metrics = savedMetrics;   memory_initialized = savedInit;
    }
};

// SsEgressFn capture sink.
struct Captured {
    int      frames = 0;
    uint32_t sourceId = 0, route = 0, seq = 0;
    std::vector<uint8_t> osc;
};
void capture(void* ctx, uint32_t sourceId, uint32_t route,
             const uint8_t* osc, uint32_t len, uint32_t seq) {
    auto* c = static_cast<Captured*>(ctx);
    c->frames++; c->sourceId = sourceId; c->route = route; c->seq = seq;
    c->osc.assign(osc, osc + len);
}

// A standalone test ring: buffer + cursors + a writer, independent of the
// arena, for pinning walker semantics on small rings where boundary
// behaviour is easy to force.
struct TestRing {
    std::vector<uint8_t>  buf;
    std::atomic<int32_t>  head{0}, tail{0}, seq{0}, lock{0};
    SsDrainState          st;

    explicit TestRing(uint32_t size) : buf(size, 0) {}
    bool write(const void* data, uint32_t len, uint32_t sourceId = 0) {
        return RingBufferWriter::write(buf.data(),
                                       static_cast<uint32_t>(buf.size()),
                                       &head, &tail, &seq, &lock,
                                       data, len, sourceId);
    }
    template <typename Fn>
    uint32_t drain(Fn&& fn, uint32_t maxFrames = 0, SsDrainStop* stop = nullptr) {
        return ss_drain_ring(buf.data(), static_cast<uint32_t>(buf.size()),
                             &head, &tail, st, SsDrainMetrics{}, maxFrames,
                             std::forward<Fn>(fn), stop);
    }
};

}  // namespace

// ── Walker semantics ─────────────────────────────────────────────────────────

TEST_CASE("walker: contiguous payload is delivered in place (no copy)", "[lanes][walker]") {
    TestRing ring(1024);
    const uint8_t msg[] = {10, 20, 30, 40};
    REQUIRE(ring.write(msg, sizeof(msg)));

    const uint8_t* seen = nullptr;
    uint32_t n = ring.drain([&](uint32_t, const uint8_t* p, uint32_t len, uint32_t) {
        seen = p;
        REQUIRE(len == sizeof(msg));
        return SsDrainVerdict::Consume;
    });
    REQUIRE(n == 1);
    // The payload pointer must lie inside the ring itself, not a copy.
    REQUIRE(seen >= ring.buf.data());
    REQUIRE(seen < ring.buf.data() + ring.buf.size());
    REQUIRE(std::memcmp(seen, msg, sizeof(msg)) == 0);
}

TEST_CASE("walker: tail advances only after the callback consumes", "[lanes][walker]") {
    TestRing ring(1024);
    const uint8_t msg[] = {1, 2, 3, 4};
    REQUIRE(ring.write(msg, sizeof(msg)));
    const int32_t tailBefore = ring.tail.load();

    ring.drain([&](uint32_t, const uint8_t*, uint32_t, uint32_t) {
        // Mid-callback the frame is still owned by the consumer: writers
        // measure free space against the tail, which must not have moved.
        REQUIRE(ring.tail.load() == tailBefore);
        return SsDrainVerdict::Consume;
    });
    REQUIRE(ring.tail.load() == ring.head.load());  // consumed after return
}

TEST_CASE("walker: Retain leaves the frame in the ring for the next drain", "[lanes][walker]") {
    TestRing ring(1024);
    const uint8_t msg[] = {'/', 'r', 0, 0};
    REQUIRE(ring.write(msg, sizeof(msg), 21));

    SsDrainStop stop;
    uint32_t n = ring.drain([](uint32_t, const uint8_t*, uint32_t, uint32_t) {
        return SsDrainVerdict::Retain;  // e.g. scheduler full
    }, 0, &stop);
    REQUIRE(n == 0);                       // retained frames are not counted
    REQUIRE(stop == SsDrainStop::Retained);
    REQUIRE(ring.tail.load() != ring.head.load());  // still queued

    // Next drain delivers the same frame intact.
    std::vector<uint8_t> got;
    uint32_t src = 0;
    n = ring.drain([&](uint32_t s, const uint8_t* p, uint32_t len, uint32_t) {
        src = s; got.assign(p, p + len);
        return SsDrainVerdict::Consume;
    }, 0, &stop);
    REQUIRE(n == 1);
    REQUIRE(src == 21);
    REQUIRE(got == std::vector<uint8_t>(msg, msg + sizeof(msg)));
    REQUIRE(ring.tail.load() == ring.head.load());
}

TEST_CASE("writer: frame that misses the boundary is padded and stays contiguous", "[lanes][walker]") {
    TestRing ring(128);

    // First frame fills 80 bytes; consume it so there is room at the front.
    uint8_t filler[64];
    std::memset(filler, 0xAA, sizeof(filler));
    REQUIRE(ring.write(filler, sizeof(filler)));
    REQUIRE(ring.drain([](uint32_t, const uint8_t*, uint32_t, uint32_t) {
        return SsDrainVerdict::Consume;
    }) == 1);

    // Second frame (64 bytes) doesn't fit in the 48 bytes before the end:
    // the writer must emit a padding marker at 80 and restart at offset 0.
    uint8_t msg[48];
    for (uint32_t i = 0; i < sizeof(msg); ++i) msg[i] = static_cast<uint8_t>(i);
    REQUIRE(ring.write(msg, sizeof(msg)));

    uint32_t padMagic;
    std::memcpy(&padMagic, ring.buf.data() + 80, sizeof(padMagic));
    REQUIRE(padMagic == PADDING_MAGIC);
    REQUIRE(ring.head.load() == 64);  // frame restarted at 0, 64 bytes long

    const uint8_t* seen = nullptr;
    std::vector<uint8_t> got;
    REQUIRE(ring.drain([&](uint32_t, const uint8_t* p, uint32_t len, uint32_t) {
        seen = p; got.assign(p, p + len);
        return SsDrainVerdict::Consume;
    }) == 1);
    REQUIRE(seen == ring.buf.data() + sizeof(Message));  // in place at offset 0
    REQUIRE(got == std::vector<uint8_t>(msg, msg + sizeof(msg)));
    REQUIRE(ring.tail.load() == ring.head.load());
}

TEST_CASE("writer: rejects a frame with free space but no contiguous room", "[lanes][walker]") {
    TestRing ring(256);
    uint8_t small[16] = {}, big[128] = {}, probe[80] = {};

    REQUIRE(ring.write(small, sizeof(small)));   // frame 32  @ 0
    REQUIRE(ring.write(big, sizeof(big)));       // frame 144 @ 32 → head 176
    REQUIRE(ring.drain([](uint32_t, const uint8_t*, uint32_t, uint32_t) {
        return SsDrainVerdict::Consume;
    }, 1) == 1);                                 // tail 32

    // 96-byte frame: 111 bytes free in total, but only 80 before the end and
    // only 31 before the tail at the front — must be rejected, head untouched.
    REQUIRE_FALSE(ring.write(probe, sizeof(probe)));
    REQUIRE(ring.head.load() == 176);
}

TEST_CASE("walker: frame claiming more bytes than published is corruption, not a stall", "[lanes][walker]") {
    TestRing ring(128);
    // Valid magic with a bit-flipped length of 64 while only 24 bytes are
    // published: waiting for the rest would wedge the lane forever (writers
    // publish head only after the complete frame).
    Message hdr;
    hdr.magic = MESSAGE_MAGIC;
    hdr.length = 64;
    hdr.sequence = 0;
    hdr.sourceId = 0;
    std::memcpy(ring.buf.data(), &hdr, sizeof(hdr));
    ring.tail.store(0);
    ring.head.store(24);

    std::atomic<uint32_t> corrupted{0};
    SsDrainMetrics m; m.corrupted = &corrupted;
    SsDrainStop stop;
    uint32_t n = ss_drain_ring(ring.buf.data(), static_cast<uint32_t>(ring.buf.size()),
                               &ring.head, &ring.tail, ring.st, m, 0,
                               [](uint32_t, const uint8_t*, uint32_t, uint32_t) {
                                   return SsDrainVerdict::Consume;
                               }, &stop);
    REQUIRE(n == 0);
    REQUIRE(stop == SsDrainStop::BadLength);
    REQUIRE(corrupted.load() == 1);
    REQUIRE(ring.tail.load() == ring.head.load());  // resynced, not waiting
}

TEST_CASE("OUT writer (ring_buffer_write) follows the unified convention: exact length, aligned advance", "[lanes][walker]") {
    // ring_buffer_write is the audio-thread OUT-ring writer; its frames are
    // drained by the same walker, so a non-4-multiple payload must round-trip
    // exactly and leave the head 4-aligned.
    std::vector<uint8_t> buf(128, 0);
    std::atomic<int32_t> head{0}, tail{0}, seq{0};
    std::atomic<uint32_t> flags{0};

    const uint8_t midi[3] = {0x90, 60, 100};  // not a 4-byte multiple
    REQUIRE(ring_buffer_write(buf.data(), 128, &head, &tail, &seq,
                              EGRESS_REPLY, 5, midi, sizeof(midi), &flags, nullptr));
    // Frame: 16 header + 4 route + 3 payload = 23 exact, 24-byte footprint.
    REQUIRE(head.load() == 24);

    SsDrainState st;
    uint32_t gotSrc = 0, gotRoute = 0;
    std::vector<uint8_t> gotOsc;
    REQUIRE(ss_drain_ring(buf.data(), 128, &head, &tail, st, SsDrainMetrics{}, 0,
        [&](uint32_t src, const uint8_t* p, uint32_t n, uint32_t) {
            gotSrc = src;
            std::memcpy(&gotRoute, p, sizeof(gotRoute));
            gotOsc.assign(p + EGRESS_ROUTE_SIZE, p + n);
            return SsDrainVerdict::Consume;
        }) == 1);
    REQUIRE(gotSrc == 5);
    REQUIRE(gotRoute == static_cast<uint32_t>(EGRESS_REPLY));
    REQUIRE(gotOsc == std::vector<uint8_t>(midi, midi + sizeof(midi)));  // exact, no pad
    REQUIRE(tail.load() == 24);  // aligned advance matches the writer
}

TEST_CASE("walker: frame crossing the ring boundary is corruption, not an OOB read", "[lanes][walker]") {
    TestRing ring(128);

    // Hand-craft a legacy/hostile wrap-split frame: valid magic at offset 80
    // with a length that runs past the end of the ring.
    Message hdr;
    hdr.magic = MESSAGE_MAGIC;
    hdr.length = 64;  // 80 + 64 > 128
    hdr.sequence = 0;
    hdr.sourceId = 0;
    std::memcpy(ring.buf.data() + 80, &hdr, sizeof(hdr));
    ring.tail.store(80);
    ring.head.store(16);  // as if the frame wrapped to 16

    std::atomic<uint32_t> corrupted{0};
    SsDrainMetrics m; m.corrupted = &corrupted;
    SsDrainStop stop;
    int delivered = 0;
    ss_drain_ring(ring.buf.data(), static_cast<uint32_t>(ring.buf.size()),
                  &ring.head, &ring.tail, ring.st, m, 0,
                  [&](uint32_t, const uint8_t*, uint32_t, uint32_t) {
                      delivered++;
                      return SsDrainVerdict::Consume;
                  }, &stop);
    REQUIRE(delivered == 0);
    REQUIRE(stop == SsDrainStop::BadLength);
    REQUIRE(corrupted.load() == 1);
    REQUIRE(ring.tail.load() == ring.head.load());  // resynced
}

TEST_CASE("writer: non-aligned payload round-trips exactly; cursors stay 4-aligned", "[lanes][walker]") {
    TestRing ring(128);
    const uint8_t odd[5] = {1, 2, 3, 4, 5};  // MIDI-style: not a 4-byte multiple
    REQUIRE(ring.write(odd, sizeof(odd)));

    std::vector<uint8_t> got;
    REQUIRE(ring.drain([&](uint32_t, const uint8_t* p, uint32_t len, uint32_t) {
        got.assign(p, p + len);
        return SsDrainVerdict::Consume;
    }) == 1);
    // Header length is exact, so the payload size round-trips untouched —
    // while the cursor advances by the 4-aligned footprint (16+5 → 24).
    REQUIRE(got == std::vector<uint8_t>({1, 2, 3, 4, 5}));
    REQUIRE(ring.tail.load() == 24);
}

TEST_CASE("walker: out-of-range tail is repaired to head", "[lanes][walker]") {
    TestRing ring(1024);
    const uint8_t msg[] = {1, 2, 3, 4};
    REQUIRE(ring.write(msg, sizeof(msg)));
    ring.tail.store(static_cast<int32_t>(ring.buf.size()) + 5);

    SsDrainStop stop;
    uint32_t n = ring.drain([](uint32_t, const uint8_t*, uint32_t, uint32_t) {
        return SsDrainVerdict::Consume;
    }, 0, &stop);
    REQUIRE(n == 0);
    REQUIRE(stop == SsDrainStop::BadCursor);
    REQUIRE(ring.tail.load() == ring.head.load());  // repaired, not dereferenced
}

TEST_CASE("walker: padding marker is followed within a single drain call", "[lanes][walker]") {
    TestRing ring(128);

    // Hand-build the never-wrap convention: padding marker near the end, a
    // real frame at offset 0 (this is the OUT-ring writer's wire format).
    const uint8_t osc[] = {'/', 'p', 0, 0};
    Message hdr;
    hdr.magic = MESSAGE_MAGIC;
    hdr.length = sizeof(Message) + sizeof(osc);
    hdr.sequence = 0;
    hdr.sourceId = 5;
    std::memcpy(ring.buf.data(), &hdr, sizeof(hdr));
    std::memcpy(ring.buf.data() + sizeof(hdr), osc, sizeof(osc));

    Message pad;
    pad.magic = PADDING_MAGIC;
    pad.length = 0;
    pad.sequence = 0;
    pad.sourceId = 0;
    std::memcpy(ring.buf.data() + 96, &pad, sizeof(pad));

    ring.tail.store(96);                                   // parked before the padding
    ring.head.store(static_cast<int32_t>(hdr.length));     // frame at 0 published

    std::vector<uint8_t> got;
    REQUIRE(ring.drain([&](uint32_t, const uint8_t* p, uint32_t len, uint32_t) {
        got.assign(p, p + len);
        return SsDrainVerdict::Consume;
    }) == 1);  // padding followed AND frame delivered in one call
    REQUIRE(got == std::vector<uint8_t>(osc, osc + sizeof(osc)));
}

TEST_CASE("walker: sequence gaps are counted by missed-frame count", "[lanes][walker]") {
    TestRing ring(1024);
    const uint8_t msg[] = {1, 2, 3, 4};
    std::atomic<uint32_t> gaps{0};
    SsDrainMetrics m; m.seqGaps = &gaps;
    auto consume = [](uint32_t, const uint8_t*, uint32_t, uint32_t) {
        return SsDrainVerdict::Consume;
    };

    REQUIRE(ring.write(msg, sizeof(msg)));   // seq 0
    ring.seq.fetch_add(3);                   // frames 1..3 lost
    REQUIRE(ring.write(msg, sizeof(msg)));   // seq 4
    ss_drain_ring(ring.buf.data(), static_cast<uint32_t>(ring.buf.size()),
                  &ring.head, &ring.tail, ring.st, m, 0, consume);
    REQUIRE(gaps.load() == 3);
}

// ── Lanes ABI ────────────────────────────────────────────────────────────────

TEST_CASE("lanes ABI: ss_ingress_write frames a message onto the IN ring", "[lanes][abi]") {
    LanesArena arena;
    const uint8_t msg[] = {1, 2, 3, 4, 5, 6, 7, 8};
    REQUIRE(ss_ingress_write(msg, sizeof(msg), 7));

    // Drain the IN ring with the shared walker; the frame must round-trip.
    SsDrainState st;
    int frames = 0; uint32_t gotSrc = 0; std::vector<uint8_t> gotOsc;
    uint32_t delivered = ss_drain_ring(
        shared_memory + IN_BUFFER_START, IN_BUFFER_SIZE,
        &control->in_head, &control->in_tail, st, SsDrainMetrics{}, 0,
        [&](uint32_t src, const uint8_t* p, uint32_t n, uint32_t) {
            frames++; gotSrc = src; gotOsc.assign(p, p + n);
            return SsDrainVerdict::Consume;
        });
    REQUIRE(delivered == 1);
    REQUIRE(frames == 1);
    REQUIRE(gotSrc == 7);
    REQUIRE(gotOsc == std::vector<uint8_t>(msg, msg + sizeof(msg)));
}

TEST_CASE("lanes ABI: NRT egress write -> drain round-trips route, token, payload", "[lanes][abi]") {
    LanesArena arena;
    const uint8_t osc[] = {'/', 'x', 0, 0};
    REQUIRE(ss_egress_nrt_write(EGRESS_BROADCAST_NOTIFY, 42, osc, sizeof(osc)));

    Captured cap;
    REQUIRE(ss_egress_nrt_drain(capture, &cap, 0) == 1);
    REQUIRE(cap.frames == 1);
    REQUIRE(cap.route == static_cast<uint32_t>(EGRESS_BROADCAST_NOTIFY));
    REQUIRE(cap.sourceId == 42);  // token rides through as the frame sourceId
    REQUIRE(cap.osc == std::vector<uint8_t>(osc, osc + sizeof(osc)));
}

TEST_CASE("lanes ABI: RT egress drain delivers a framed OUT frame", "[lanes][abi]") {
    LanesArena arena;
    // RT egress is written inside ss_tick; inject one [route][osc] frame directly.
    std::atomic<int32_t> lock{0};
    uint8_t framed[EGRESS_ROUTE_SIZE + 4];
    uint32_t route = EGRESS_REPLY;
    std::memcpy(framed, &route, sizeof(route));
    std::memcpy(framed + EGRESS_ROUTE_SIZE, "/ok", 4);
    REQUIRE(RingBufferWriter::write(
        shared_memory + OUT_BUFFER_START, OUT_BUFFER_SIZE,
        &control->out_head, &control->out_tail, &control->out_sequence, &lock,
        framed, sizeof(framed), 99));

    Captured cap;
    REQUIRE(ss_egress_rt_drain(capture, &cap, 0) == 1);
    REQUIRE(cap.route == static_cast<uint32_t>(EGRESS_REPLY));
    REQUIRE(cap.sourceId == 99);
    REQUIRE(std::string(reinterpret_cast<const char*>(cap.osc.data())) == "/ok");
}

TEST_CASE("lanes ABI: ingress/egress reject bad input and empty drains", "[lanes][abi]") {
    LanesArena arena;
    REQUIRE_FALSE(ss_ingress_write(nullptr, 4, 0));
    REQUIRE_FALSE(ss_ingress_write(reinterpret_cast<const uint8_t*>("x"), 0, 0));

    Captured cap;
    REQUIRE(ss_egress_rt_drain(capture, &cap, 0) == 0);   // nothing queued
    REQUIRE(ss_egress_nrt_drain(capture, &cap, 0) == 0);
    REQUIRE(cap.frames == 0);
}
