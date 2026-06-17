/*
 * test_metrics.cpp — Coverage for PerformanceMetrics.
 *
 * For each field one or more tests verify:
 *   (a) the field is readable via SupersonicEngine::getMetrics()
 *   (b) the field has a sensible value at boot
 *   (c) the field increments on its native write path
 *
 * Some fields are JS-only (no native writer): wasm_errors and
 * osc_in_dropped_messages. Their tests assert the value stays 0 on native
 * and are tagged [metrics][js-only] for easy filtering.
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

TEST_CASE("metrics: nrt_out_buffer_used_bytes < NRT_OUT_BUFFER_SIZE",
          "[metrics][buffer]") {
    EngineFixture fx;
    CHECK(metrics(fx).nrt_out_buffer_used_bytes.load() < NRT_OUT_BUFFER_SIZE);
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
    // The reply guarantees a reply was written to the OUT buffer, but the peak
    // is stored by the audio thread with memory_order_relaxed (unordered with
    // the reply), so on weak-memory arm64 it can lag. Poll until visible.
    CHECK(fx.pollUntil([&] {
        return metrics(fx).out_buffer_peak_bytes.load() > 0;
    }));
}

TEST_CASE("metrics: nrt_out_buffer_peak_bytes is readable",
          "[metrics][buffer]") {
    EngineFixture fx;
    (void)metrics(fx).nrt_out_buffer_peak_bytes.load();
    SUCCEED();
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
