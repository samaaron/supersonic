/*
 * test_ring_concurrency.cpp — true multi-thread coverage for the ingress/egress
 * rings.
 *
 * The rest of the ring suite (test_ring_buffer_write, test_lanes, test_in_ring_
 * drain, test_ring_wire_conformance) is thorough but single-threaded: it proves
 * framing, wraparound, corruption repair and overflow accounting, never real
 * producer/consumer contention. This file closes that gap and, under the
 * sanitize-tsan build, turns each ring's threading assumption into a checkable
 * fact.
 *
 * Catch2 assertion macros are NOT thread-safe, so worker threads only touch
 * plain data / atomics; every REQUIRE/CHECK runs on the main thread after join.
 *
 * ── Layout ────────────────────────────────────────────────────────────────
 *   [RingConcurrency]  runs in CI (incl. TSan). GREEN regression guards: (a) the
 *                      shared multi-producer writer `RingBufferWriter::write`
 *                      (behind BOTH the ingress ring / in_write_lock and the
 *                      NRT-egress ring / g_nrt_egress_lock) under contention, and
 *                      (b) off-audio-thread debug routing to NRT-out, which keeps
 *                      the RT-out ring single-writer.
 *   [.][ring-todo]     hidden (excluded from the default/CI run, so main stays
 *                      green). Each reproduces one still-open defect; running it
 *                      explicitly under TSan shows the race. Flips green when the
 *                      corresponding fix lands.
 */
#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <thread>
#include <vector>

#include "shared_memory.h"             // ControlPointers layout, EgressRoute, PerformanceMetrics
#include "ring/ring.h"                 // Message, MESSAGE_MAGIC
#include "workers/RingBufferWriter.h"  // shared MPSC writer (ingress + NRT-egress)
#include "lanes/ring_drain.h"          // ss_drain_ring consumer

// Engine globals for the debug-egress routing test (audio_processor.cpp). Declared
// directly rather than via audio_processor.h, which pulls in <emscripten/...>.
extern "C" int ss_log(const char* fmt, ...);
extern "C" {
    extern uint8_t*            shared_memory;
    extern ControlPointers*    control;
    extern PerformanceMetrics* metrics;
    extern bool                memory_initialized;
}
extern std::atomic<bool> g_nrt_egress_drained;   // NRT-out drainer present (capability)

namespace {

// 8-byte payload identifying (producer, sequence-within-producer) so the drainer
// can prove every frame arrived exactly once with its bytes intact.
struct Tag { uint32_t producer; uint32_t seq; };

inline void packTag(uint8_t out[8], uint32_t producer, uint32_t seq) {
    std::memcpy(out,     &producer, 4);
    std::memcpy(out + 4, &seq,      4);
}

}  // namespace

// ─────────────────────────────────────────────────────────────────────────────
// Coverage: the shared multi-producer writer under real contention (GREEN).
// N producer threads race RingBufferWriter::write into one ring while a single
// consumer drains it; every distinct frame must arrive exactly once, uncorrupted.
// This is the in_write_lock / g_nrt_egress_lock path, otherwise untested.
// ─────────────────────────────────────────────────────────────────────────────
TEST_CASE("MPSC ring: concurrent producers and a drainer lose no frames",
          "[RingConcurrency]") {
    constexpr uint32_t kProducers   = 4;
    constexpr uint32_t kPerProducer = 4000;
    constexpr uint32_t kTotal       = kProducers * kPerProducer;

    std::vector<uint8_t> ring(64 * 1024, 0);
    std::atomic<int32_t> head{0}, tail{0}, sequence{0}, writeLock{0};
    std::atomic<bool>    go{false};
    std::atomic<bool>    producerStuck{false};
    std::atomic<uint32_t> received{0};

    // Owned exclusively by the consumer thread → no synchronisation needed; read
    // on the main thread only after join() establishes happens-before.
    std::vector<uint8_t> seen(kTotal, 0);
    uint32_t duplicates = 0;
    uint32_t badPayload = 0;

    std::thread consumer([&] {
        SsDrainState  st;
        SsDrainMetrics m{};
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        while (received.load(std::memory_order_relaxed) < kTotal
               && std::chrono::steady_clock::now() < deadline) {
            ss_drain_ring(ring.data(), static_cast<uint32_t>(ring.size()),
                          &head, &tail, st, m, 0,
                [&](uint32_t /*sourceId*/, const uint8_t* payload,
                    uint32_t n, uint32_t /*seq*/) {
                    if (n != sizeof(Tag)) { ++badPayload; return SsDrainVerdict::Consume; }
                    Tag t;
                    std::memcpy(&t, payload, sizeof(t));
                    if (t.producer < kProducers && t.seq < kPerProducer) {
                        const uint32_t idx = t.producer * kPerProducer + t.seq;
                        if (seen[idx]) ++duplicates; else seen[idx] = 1;
                    } else {
                        ++badPayload;
                    }
                    received.fetch_add(1, std::memory_order_relaxed);
                    return SsDrainVerdict::Consume;
                });
            std::this_thread::yield();
        }
    });

    std::vector<std::thread> producers;
    for (uint32_t p = 0; p < kProducers; ++p) {
        producers.emplace_back([&, p] {
            while (!go.load(std::memory_order_acquire)) { /* start gate: maximise overlap */ }
            for (uint32_t s = 0; s < kPerProducer; ++s) {
                uint8_t payload[sizeof(Tag)];
                packTag(payload, p, s);
                const auto deadline =
                    std::chrono::steady_clock::now() + std::chrono::seconds(30);
                while (!RingBufferWriter::write(
                           ring.data(), static_cast<uint32_t>(ring.size()),
                           &head, &tail, &sequence, &writeLock,
                           payload, sizeof(payload), p)) {
                    // Ring transiently full; the drainer will free space. Bounded so
                    // a real stall fails loudly instead of hanging the suite.
                    if (std::chrono::steady_clock::now() > deadline) {
                        producerStuck.store(true);
                        return;
                    }
                    std::this_thread::yield();
                }
            }
        });
    }

    go.store(true, std::memory_order_release);
    for (auto& t : producers) t.join();
    consumer.join();

    REQUIRE_FALSE(producerStuck.load());
    CHECK(received.load() == kTotal);
    CHECK(duplicates == 0);
    CHECK(badPayload == 0);
    uint32_t missing = 0;
    for (uint8_t v : seen) if (!v) ++missing;
    CHECK(missing == 0);
}

// ─────────────────────────────────────────────────────────────────────────────
// Defect #1 fix guard (RUNS, GREEN): off-audio-thread debug routes to NRT-out.
// RT-out (ring_buffer_write) is lock-free — safe ONLY with the audio thread as its
// sole writer. Off the audio thread, emit_debug_osc must route to the locked
// NRT-out ring instead, so RT-out never gets a second writer. This drives ss_log
// from the (non-audio) test thread and asserts the frame lands in NRT-out and
// leaves RT-out untouched; and that with no drainer (worklet target) it falls back
// to RT-out. Deterministic, no gateway — before the fix ss_log always hit RT-out.
// ─────────────────────────────────────────────────────────────────────────────
TEST_CASE("off-audio-thread ss_log routes to NRT-out, not the RT-out ring",
          "[RingConcurrency]") {
    // Self-contained arena + save/restore, so the test neither depends on engine
    // init nor disturbs global state for other tests.
    static std::vector<uint8_t> arena(TOTAL_BUFFER_SIZE, 0);
    uint8_t*            savedSM   = shared_memory;
    ControlPointers*    savedC    = control;
    PerformanceMetrics* savedM    = metrics;
    const bool          savedInit = memory_initialized;
    const bool          savedDrn  = g_nrt_egress_drained.load(std::memory_order_relaxed);

    shared_memory = arena.data();
    control       = reinterpret_cast<ControlPointers*>(arena.data() + CONTROL_START);
    metrics       = nullptr;   // emit_debug_osc guards on this; keeps the arena minimal
    ControlPointers* c = control;
    c->out_head.store(0);      c->out_tail.store(0);      c->out_sequence.store(0);
    c->nrt_out_head.store(0);  c->nrt_out_tail.store(0);  c->nrt_out_sequence.store(0);
    memory_initialized = true;

    // Drainer present (native NRT gateway): off-thread debug goes to NRT-out and
    // leaves the single-writer RT-out ring untouched.
    g_nrt_egress_drained.store(true, std::memory_order_relaxed);
    ss_log("ring-route-test off-audio-thread line");
    CHECK(c->out_head.load()     == 0);   // RT-out untouched
    CHECK(c->nrt_out_head.load() != 0);   // NRT-out advanced

    // No drainer (worklet target): falls back to the always-safe RT-out ring.
    g_nrt_egress_drained.store(false, std::memory_order_relaxed);
    ss_log("ring-route-test worklet-fallback line");
    CHECK(c->out_head.load() != 0);       // RT-out advanced

    shared_memory = savedSM; control = savedC; metrics = savedM;
    memory_initialized = savedInit;
    g_nrt_egress_drained.store(savedDrn, std::memory_order_relaxed);
}

// ─────────────────────────────────────────────────────────────────────────────
// Defect #2 (HIDDEN, RED): rebuild reassigning the ring base racing a live writer.
// On a device rebuild, init_memory reassigns the NON-ATOMIC globals shared_memory
// / control / metrics (audio_processor.cpp:457) while off-thread users still
// dereference them — the same class as defect #1. (The atomic head/tail/sequence
// zeroing is only a logical race the drain repairs, and atomics never trip TSan,
// so the detectable hazard is the plain-pointer reassignment.) g_external_segment
// is stable across rebuilds, so the reassign writes the SAME value — benign in
// value, still an unsynchronised non-atomic write racing a read. Modeled here
// with a non-atomic base pointer a "rebuild" thread reassigns while a writer
// dereferences it. Fix: quiesce writers before the rebuild reassigns the globals.
// ─────────────────────────────────────────────────────────────────────────────
TEST_CASE("rebuild reassigning the ring base races a live writer",
          "[.][ring-todo]") {
    std::vector<uint8_t> ring(16 * 1024, 0);
    uint8_t* base = ring.data();   // models the non-atomic `shared_memory` global
    std::atomic<int32_t> head{0}, tail{0}, sequence{0}, writeLock{0};
    std::atomic<bool>    stop{false};

    std::thread writer([&] {
        uint32_t s = 0;
        while (!stop.load(std::memory_order_acquire)) {
            uint8_t payload[sizeof(Tag)];
            packTag(payload, 0, s++);
            uint8_t* b = base;   // non-atomic read of the base — races the reassign
            RingBufferWriter::write(b, static_cast<uint32_t>(ring.size()),
                                    &head, &tail, &sequence, &writeLock,
                                    payload, sizeof(payload), 0);
        }
    });
    std::thread rebuild([&] {
        while (!stop.load(std::memory_order_acquire)) {
            base = ring.data();   // init_memory: shared_memory = g_external_segment (same value)
            std::this_thread::yield();
        }
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    stop.store(true, std::memory_order_release);
    writer.join();
    rebuild.join();

    SUCCEED("a writer ran against a concurrent base-pointer reassign; under TSan "
            "this reports the rebuild-vs-writer race (defect #2)");
}

// ─────────────────────────────────────────────────────────────────────────────
// Defect #3 (HIDDEN, RED): stuck lock / the IN-ring hang.
// write_lock is a plain word with no owner or deadline. If a producer dies
// holding it, the next producer spins unbounded. Here the lock is pre-held
// (owner "died"); a writer must still return within a deadline. Today it spins
// forever → `completedInTime` is false → RED. A bounded-spin fix (give up and
// return rather than block) flips it green.
// ─────────────────────────────────────────────────────────────────────────────
TEST_CASE("a producer must not spin forever on a lock held by a dead owner",
          "[.][ring-todo]") {
    std::vector<uint8_t> ring(4 * 1024, 0);
    std::atomic<int32_t> head{0}, tail{0}, sequence{0};
    std::atomic<int32_t> writeLock{1};   // owner died holding the lock
    std::atomic<bool>    completed{false};

    std::thread w([&] {
        uint8_t payload[sizeof(Tag)];
        packTag(payload, 0, 0);
        RingBufferWriter::write(ring.data(), static_cast<uint32_t>(ring.size()),
                                &head, &tail, &sequence, &writeLock,
                                payload, sizeof(payload), 0);
        completed.store(true, std::memory_order_release);
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    const bool completedInTime = completed.load(std::memory_order_acquire);

    // Release the lock so an unfixed (still-spinning) writer can finish — the test
    // must never hang at join(), even while demonstrating the defect.
    writeLock.store(0, std::memory_order_release);
    w.join();

    CHECK(completedInTime);   // RED today (unbounded spin); GREEN with a bounded-spin fix
}
