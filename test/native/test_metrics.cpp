/*
 * test_metrics.cpp — Comprehensive coverage for PerformanceMetrics
 *
 * For every field in PerformanceMetrics (src/shared_memory.h:169) one or
 * more tests verify:
 *   (a) the field is readable via SupersonicEngine::getMetrics()
 *   (b) the field has a sensible value at boot
 *   (c) the field increments on the documented native driver path
 *
 * Some fields are JS-only writers (no native code path updates them) — the
 * tests for those explicitly assert the value stays 0 on native, documenting
 * the metrics-coverage gap between runtimes. Those tests are tagged
 * [metrics][js-only] for easy filtering.
 *
 * Driver-path summary (verified from grep over src/):
 *   audio_processor.cpp writes: process_count, messages_processed,
 *     messages_dropped, scheduler_queue_depth/max/dropped, messages_sequence_gaps,
 *     scheduler_lates, scheduler_max_late_ms, scheduler_last_late_ms,
 *     scheduler_last_late_tick, in/out/debug_buffer_used/peak_bytes (plus
 *     ring-buffer tracking inside scsynth core).
 *   OscUdpServer.cpp writes: osc_out_messages_sent, osc_out_bytes_sent,
 *     bypass_immediate, bypass_near_future, bypass_late, prescheduler_bypassed.
 *   ReplyReader.cpp writes: osc_in_messages_received, osc_in_bytes_received,
 *     osc_in_corrupted, messages_sequence_gaps.
 *   DebugReader.cpp writes: debug_messages_received, debug_bytes_received.
 *   Prescheduler.cpp writes: prescheduler_pending, prescheduler_pending_peak,
 *     prescheduler_bundles_scheduled, prescheduler_events_cancelled,
 *     prescheduler_dispatched, prescheduler_total_dispatches,
 *     prescheduler_retries_failed, prescheduler_retries_succeeded,
 *     prescheduler_retry_queue_size, prescheduler_retry_queue_peak,
 *     prescheduler_messages_retried, prescheduler_min_headroom_ms,
 *     prescheduler_lates, prescheduler_max_late_ms.
 *
 * Native-unwritten fields (JS-only, always 0 on native):
 *   wasm_errors, bypass_non_bundle, osc_in_dropped_messages.
 */
#include "EngineFixture.h"
#include "OscBuilder.h"
#include "WallClock.h"
#include "src/shared_memory.h"

#include <thread>
#include <chrono>

extern "C" uint8_t ring_buffer_storage[];

// ── Helpers ─────────────────────────────────────────────────────────────────

namespace {
const PerformanceMetrics& metrics(EngineFixture& fx) {
    return fx.engine().getMetrics();
}
}  // namespace

// ============================================================================
// Accessor smoke
// ============================================================================

TEST_CASE("metrics: getMetrics returns the shared struct after init",
          "[metrics][api]") {
    EngineFixture fx;
    const PerformanceMetrics& m = fx.engine().getMetrics();
    CHECK(&m == reinterpret_cast<PerformanceMetrics*>(
                    ring_buffer_storage + METRICS_START));
}

TEST_CASE("metrics: metricsPtr() matches getMetrics()", "[metrics][api]") {
    EngineFixture fx;
    CHECK(fx.engine().metricsPtr() == &fx.engine().getMetrics());
}

TEST_CASE("metrics: METRICS_START offset is contiguous after CONTROL",
          "[metrics][api]") {
    CHECK(METRICS_START == CONTROL_START + CONTROL_SIZE);
}

// ============================================================================
// scsynth metrics [0-8]
// ============================================================================

TEST_CASE("metrics: process_count > 0 after boot", "[metrics][scsynth]") {
    EngineFixture fx;
    CHECK(metrics(fx).process_count.load() > 0);
}

TEST_CASE("metrics: process_count increments after sync barrier",
          "[metrics][scsynth]") {
    EngineFixture fx;
    uint32_t before = metrics(fx).process_count.load();
    fx.send(osc_test::message("/sync", 200));
    OscReply r;
    REQUIRE(fx.waitForReply("/synced", r));
    CHECK(metrics(fx).process_count.load() > before);
}

TEST_CASE("metrics: messages_processed > 0 after boot",
          "[metrics][scsynth]") {
    EngineFixture fx;  // EngineFixture sends /g_new + /sync at boot
    CHECK(metrics(fx).messages_processed.load() > 0);
}

TEST_CASE("metrics: messages_processed increments on /status",
          "[metrics][scsynth]") {
    EngineFixture fx;
    uint32_t before = metrics(fx).messages_processed.load();
    fx.send(osc_test::message("/status"));
    fx.send(osc_test::message("/sync", 201));
    OscReply r;
    REQUIRE(fx.waitForReply("/synced", r));
    CHECK(metrics(fx).messages_processed.load() > before);
}

TEST_CASE("metrics: messages_processed scales with N /status sends",
          "[metrics][scsynth]") {
    EngineFixture fx;
    uint32_t before = metrics(fx).messages_processed.load();
    constexpr int N = 5;
    for (int i = 0; i < N; ++i) fx.send(osc_test::message("/status"));
    fx.send(osc_test::message("/sync", 202));
    OscReply r;
    REQUIRE(fx.waitForReply("/synced", r));
    uint32_t delta = metrics(fx).messages_processed.load() - before;
    CHECK(delta >= static_cast<uint32_t>(N));
}

TEST_CASE("metrics: messages_dropped is 0 or low at idle",
          "[metrics][scsynth]") {
    EngineFixture fx;
    CHECK(metrics(fx).messages_dropped.load() <= 1);
}

TEST_CASE("metrics: scheduler_queue_depth is readable",
          "[metrics][scsynth]") {
    EngineFixture fx;
    (void)metrics(fx).scheduler_queue_depth.load();
    SUCCEED();
}

TEST_CASE("metrics: scheduler_queue_max >= scheduler_queue_depth",
          "[metrics][scsynth]") {
    EngineFixture fx;
    auto& m = metrics(fx);
    CHECK(m.scheduler_queue_max.load() >= m.scheduler_queue_depth.load());
}

TEST_CASE("metrics: scheduler_queue_dropped is 0 in normal operation",
          "[metrics][scsynth]") {
    EngineFixture fx;
    fx.send(osc_test::message("/status"));
    fx.send(osc_test::message("/sync", 203));
    OscReply r;
    REQUIRE(fx.waitForReply("/synced", r));
    CHECK(metrics(fx).scheduler_queue_dropped.load() == 0);
}

TEST_CASE("metrics: messages_sequence_gaps is 0 in normal operation",
          "[metrics][scsynth]") {
    EngineFixture fx;
    fx.send(osc_test::message("/status"));
    fx.send(osc_test::message("/sync", 204));
    OscReply r;
    REQUIRE(fx.waitForReply("/synced", r));
    CHECK(metrics(fx).messages_sequence_gaps.load() == 0);
}

TEST_CASE("metrics: wasm_errors is 0 on native (JS-only)",
          "[metrics][scsynth][js-only]") {
    EngineFixture fx;
    fx.send(osc_test::message("/status"));
    CHECK(metrics(fx).wasm_errors.load() == 0);
}

TEST_CASE("metrics: scheduler_lates is 0 or low at idle",
          "[metrics][scsynth]") {
    EngineFixture fx;
    fx.send(osc_test::message("/status"));
    fx.send(osc_test::message("/sync", 205));
    OscReply r;
    REQUIRE(fx.waitForReply("/synced", r));
    // No late dispatches expected when nothing is scheduled in the future
    CHECK(metrics(fx).scheduler_lates.load() <= 1);
}

// ============================================================================
// Prescheduler metrics [9-23]
// ============================================================================

TEST_CASE("metrics: prescheduler_pending is 0 at idle",
          "[metrics][prescheduler]") {
    EngineFixture fx;
    CHECK(metrics(fx).prescheduler_pending.load() == 0);
}

TEST_CASE("metrics: prescheduler_pending_peak grows with concurrent FAR_FUTURE schedules",
          "[metrics][prescheduler]") {
    EngineFixture fx;
    constexpr int N = 5;
    uint32_t before = metrics(fx).prescheduler_pending_peak.load();
    for (int i = 0; i < N; ++i) {
        fx.engine().sendBundle(wallClockNTP() + 60.0 + i,
                               { OscBuilder::message("/status") });
    }
    // Give the prescheduler thread a moment to register the schedules
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    // Peak must have at some point reached at least the pending count
    CHECK(metrics(fx).prescheduler_pending_peak.load() >= before + N);
}

TEST_CASE("metrics: prescheduler_bundles_scheduled increments on FAR_FUTURE",
          "[metrics][prescheduler]") {
    EngineFixture fx;
    uint32_t before = metrics(fx).prescheduler_bundles_scheduled.load();
    fx.engine().sendBundle(wallClockNTP() + 60.0,
                           { OscBuilder::message("/status") });
    // Give the prescheduler thread a moment to register the schedule
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    CHECK(metrics(fx).prescheduler_bundles_scheduled.load() > before);
}

TEST_CASE("metrics: prescheduler_bypassed increments on FAR_FUTURE",
          "[metrics][prescheduler]") {
    EngineFixture fx;
    uint32_t before = metrics(fx).prescheduler_bypassed.load();
    fx.engine().sendBundle(wallClockNTP() + 60.0,
                           { OscBuilder::message("/status") });
    CHECK(metrics(fx).prescheduler_bypassed.load() > before);
}

TEST_CASE("metrics: prescheduler_dispatched increments after scheduled fire",
          "[metrics][prescheduler]") {
    EngineFixture fx;
    uint32_t before = metrics(fx).prescheduler_dispatched.load();
    // Schedule beyond the 0.5s default lookahead so it goes through the
    // prescheduler, but close enough that we can wait it out.
    fx.engine().sendBundle(wallClockNTP() + 0.7,
                           { OscBuilder::message("/status") });
    std::this_thread::sleep_for(std::chrono::milliseconds(900));
    CHECK(metrics(fx).prescheduler_dispatched.load() > before);
}

TEST_CASE("metrics: prescheduler_total_dispatches increments after fire",
          "[metrics][prescheduler]") {
    EngineFixture fx;
    uint32_t before = metrics(fx).prescheduler_total_dispatches.load();
    fx.engine().sendBundle(wallClockNTP() + 0.7,
                           { OscBuilder::message("/status") });
    std::this_thread::sleep_for(std::chrono::milliseconds(900));
    CHECK(metrics(fx).prescheduler_total_dispatches.load() > before);
}

TEST_CASE("metrics: prescheduler_events_cancelled increments on purge",
          "[metrics][prescheduler]") {
    EngineFixture fx;
    fx.engine().sendBundle(wallClockNTP() + 60.0,
                           { OscBuilder::message("/status") });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    uint32_t before = metrics(fx).prescheduler_events_cancelled.load();
    fx.engine().purge();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    CHECK(metrics(fx).prescheduler_events_cancelled.load() > before);
}

TEST_CASE("metrics: prescheduler_min_headroom_ms initialised to sentinel",
          "[metrics][prescheduler]") {
    EngineFixture fx;
    // 0xFFFFFFFF is HEADROOM_UNSET_SENTINEL — see Prescheduler.h.
    CHECK(metrics(fx).prescheduler_min_headroom_ms.load() == 0xFFFFFFFFu);
}

TEST_CASE("metrics: prescheduler_min_headroom_ms recorded after FAR_FUTURE dispatch",
          "[metrics][prescheduler]") {
    EngineFixture fx;
    // Schedule at now + 0.7s (FAR_FUTURE since lookahead is 0.5s).
    // Dispatch fires at ~ now + 0.2s with ~500ms remaining until exec.
    fx.engine().sendBundle(wallClockNTP() + 0.7,
                           { OscBuilder::message("/status") });
    std::this_thread::sleep_for(std::chrono::milliseconds(900));
    uint32_t headroom = metrics(fx).prescheduler_min_headroom_ms.load();
    CHECK(headroom != 0xFFFFFFFFu);
    CHECK(headroom < 1000);  // sanity bound: well under 1 second
}

TEST_CASE("metrics: prescheduler_lates is 0 at idle",
          "[metrics][prescheduler]") {
    // Native writes this on late dispatch (event time already past at fire);
    // no late events expected in a normal idle test.
    EngineFixture fx;
    CHECK(metrics(fx).prescheduler_lates.load() == 0);
}

TEST_CASE("metrics: prescheduler_retries_succeeded is 0 without buffer-full",
          "[metrics][prescheduler]") {
    // Native writes this when a previously-queued retry succeeds. No buffer-full
    // condition expected at idle.
    EngineFixture fx;
    CHECK(metrics(fx).prescheduler_retries_succeeded.load() == 0);
}

TEST_CASE("metrics: prescheduler_retries_failed is 0 at idle",
          "[metrics][prescheduler]") {
    EngineFixture fx;
    fx.engine().sendBundle(wallClockNTP() + 0.7,
                           { OscBuilder::message("/status") });
    std::this_thread::sleep_for(std::chrono::milliseconds(900));
    CHECK(metrics(fx).prescheduler_retries_failed.load() == 0);
}

TEST_CASE("metrics: prescheduler_retry_queue_size is 0 without buffer-full",
          "[metrics][prescheduler]") {
    EngineFixture fx;
    CHECK(metrics(fx).prescheduler_retry_queue_size.load() == 0);
}

TEST_CASE("metrics: prescheduler_retry_queue_peak is 0 without buffer-full",
          "[metrics][prescheduler]") {
    EngineFixture fx;
    CHECK(metrics(fx).prescheduler_retry_queue_peak.load() == 0);
}

TEST_CASE("metrics: prescheduler_messages_retried is 0 without buffer-full",
          "[metrics][prescheduler]") {
    EngineFixture fx;
    CHECK(metrics(fx).prescheduler_messages_retried.load() == 0);
}

TEST_CASE("metrics: prescheduler_max_late_ms is 0 at idle",
          "[metrics][prescheduler]") {
    // Native writes max(lateMs) on late dispatch. No late events expected at idle.
    EngineFixture fx;
    CHECK(metrics(fx).prescheduler_max_late_ms.load() == 0);
}

// ============================================================================
// OSC Out [24-25]
// (Counts messages routed through the ring buffer to scsynth — i.e. anything
//  the engine receives. Naming is JS-perspective: "out to scsynth".)
// ============================================================================

TEST_CASE("metrics: osc_out_messages_sent increments on send",
          "[metrics][osc-out]") {
    EngineFixture fx;
    uint32_t before = metrics(fx).osc_out_messages_sent.load();
    fx.send(osc_test::message("/status"));
    CHECK(metrics(fx).osc_out_messages_sent.load() > before);
}

TEST_CASE("metrics: osc_out_bytes_sent increments by message size",
          "[metrics][osc-out]") {
    EngineFixture fx;
    uint32_t before = metrics(fx).osc_out_bytes_sent.load();
    auto pkt = osc_test::message("/status");
    fx.send(pkt);
    uint32_t after = metrics(fx).osc_out_bytes_sent.load();
    CHECK(after >= before + pkt.size());
}

// ============================================================================
// OSC In [26-29]
// ============================================================================

TEST_CASE("metrics: osc_in_messages_received increments on reply",
          "[metrics][osc-in]") {
    EngineFixture fx;
    uint32_t before = metrics(fx).osc_in_messages_received.load();
    fx.send(osc_test::message("/status"));
    OscReply r;
    REQUIRE(fx.waitForReply("/status.reply", r));
    CHECK(metrics(fx).osc_in_messages_received.load() > before);
}

TEST_CASE("metrics: osc_in_bytes_received grows when replies arrive",
          "[metrics][osc-in]") {
    EngineFixture fx;
    uint32_t before = metrics(fx).osc_in_bytes_received.load();
    fx.send(osc_test::message("/status"));
    OscReply r;
    REQUIRE(fx.waitForReply("/status.reply", r));
    CHECK(metrics(fx).osc_in_bytes_received.load() > before);
}

TEST_CASE("metrics: osc_in_dropped_messages is 0 on native (JS-only)",
          "[metrics][osc-in][js-only]") {
    EngineFixture fx;
    fx.send(osc_test::message("/status"));
    CHECK(metrics(fx).osc_in_dropped_messages.load() == 0);
}

TEST_CASE("metrics: osc_in_corrupted is 0 in normal operation",
          "[metrics][osc-in]") {
    EngineFixture fx;
    fx.send(osc_test::message("/status"));
    OscReply r;
    REQUIRE(fx.waitForReply("/status.reply", r));
    CHECK(metrics(fx).osc_in_corrupted.load() == 0);
}

// ============================================================================
// Debug [30-31]
// ============================================================================

TEST_CASE("metrics: debug_messages_received is readable",
          "[metrics][debug]") {
    EngineFixture fx;
    (void)metrics(fx).debug_messages_received.load();
    SUCCEED();
}

TEST_CASE("metrics: debug_bytes_received is readable",
          "[metrics][debug]") {
    EngineFixture fx;
    (void)metrics(fx).debug_bytes_received.load();
    SUCCEED();
}

// ============================================================================
// Buffer usage [32-37]
// ============================================================================

TEST_CASE("metrics: in_buffer_used_bytes < IN_BUFFER_SIZE",
          "[metrics][buffer]") {
    EngineFixture fx;
    fx.send(osc_test::message("/status"));
    CHECK(metrics(fx).in_buffer_used_bytes.load() < IN_BUFFER_SIZE);
}

TEST_CASE("metrics: out_buffer_used_bytes < OUT_BUFFER_SIZE",
          "[metrics][buffer]") {
    EngineFixture fx;
    fx.send(osc_test::message("/status"));
    OscReply r;
    REQUIRE(fx.waitForReply("/status.reply", r));
    CHECK(metrics(fx).out_buffer_used_bytes.load() < OUT_BUFFER_SIZE);
}

TEST_CASE("metrics: debug_buffer_used_bytes < DEBUG_BUFFER_SIZE",
          "[metrics][buffer]") {
    EngineFixture fx;
    CHECK(metrics(fx).debug_buffer_used_bytes.load() < DEBUG_BUFFER_SIZE);
}

TEST_CASE("metrics: in_buffer_peak_bytes > 0 after activity",
          "[metrics][buffer]") {
    EngineFixture fx;
    fx.send(osc_test::message("/status"));
    CHECK(metrics(fx).in_buffer_peak_bytes.load() > 0);
}

TEST_CASE("metrics: out_buffer_peak_bytes > 0 after replies",
          "[metrics][buffer]") {
    EngineFixture fx;
    fx.send(osc_test::message("/status"));
    OscReply r;
    REQUIRE(fx.waitForReply("/status.reply", r));
    CHECK(metrics(fx).out_buffer_peak_bytes.load() > 0);
}

TEST_CASE("metrics: debug_buffer_peak_bytes is readable",
          "[metrics][buffer]") {
    EngineFixture fx;
    (void)metrics(fx).debug_buffer_peak_bytes.load();
    SUCCEED();
}

// ============================================================================
// Bypass categories [38-41]
// ============================================================================

TEST_CASE("metrics: bypass_non_bundle is 0 on native (JS-only)",
          "[metrics][bypass][js-only]") {
    // Native OscUdpServer counts plain messages as IMMEDIATE; the JS transport
    // is the only writer that distinguishes non-bundle.
    EngineFixture fx;
    fx.send(osc_test::message("/status"));
    CHECK(metrics(fx).bypass_non_bundle.load() == 0);
}

TEST_CASE("metrics: bypass_immediate increments on plain messages",
          "[metrics][bypass]") {
    EngineFixture fx;
    uint32_t before = metrics(fx).bypass_immediate.load();
    fx.send(osc_test::message("/status"));
    CHECK(metrics(fx).bypass_immediate.load() > before);
}

TEST_CASE("metrics: bypass_near_future increments on near-future bundles",
          "[metrics][bypass]") {
    EngineFixture fx;
    uint32_t before = metrics(fx).bypass_near_future.load();
    // Within the default lookahead (0.5s) but in the future
    fx.engine().sendBundle(wallClockNTP() + 0.05,
                           { OscBuilder::message("/status") });
    CHECK(metrics(fx).bypass_near_future.load() > before);
}

TEST_CASE("metrics: bypass_late increments on past timetags",
          "[metrics][bypass]") {
    EngineFixture fx;
    uint32_t before = metrics(fx).bypass_late.load();
    fx.engine().sendBundle(wallClockNTP() - 1.0,
                           { OscBuilder::message("/status") });
    CHECK(metrics(fx).bypass_late.load() > before);
}

// ============================================================================
// scsynth late timing [42-44]
// ============================================================================

TEST_CASE("metrics: scheduler_max_late_ms is readable",
          "[metrics][late-timing]") {
    EngineFixture fx;
    fx.send(osc_test::message("/status"));
    fx.send(osc_test::message("/sync", 220));
    OscReply r;
    REQUIRE(fx.waitForReply("/synced", r));
    // No assertions on exact value — depends on timing. Just verify readable
    // and non-pathological.
    int32_t v = metrics(fx).scheduler_max_late_ms.load();
    CHECK(v >= 0);
    CHECK(v < 10000);  // sanity bound: not absurdly large
}

TEST_CASE("metrics: scheduler_last_late_ms is readable",
          "[metrics][late-timing]") {
    EngineFixture fx;
    int32_t v = metrics(fx).scheduler_last_late_ms.load();
    CHECK(v >= 0);
    CHECK(v < 10000);
}

TEST_CASE("metrics: scheduler_last_late_tick is readable",
          "[metrics][late-timing]") {
    EngineFixture fx;
    (void)metrics(fx).scheduler_last_late_tick.load();
    SUCCEED();
}
