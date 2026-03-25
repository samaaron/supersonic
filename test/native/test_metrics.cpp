/*
 * test_metrics.cpp — PerformanceMetrics from shared memory
 */
#include "EngineFixture.h"
#include "OscBuilder.h"
#include "WallClock.h"
#include "src/shared_memory.h"

extern "C" uint8_t ring_buffer_storage[];

TEST_CASE("process_count increments after pump", "[metrics]") {
    EngineFixture fx;
    auto* metrics = reinterpret_cast<PerformanceMetrics*>(ring_buffer_storage + METRICS_START);

    uint32_t before = metrics->process_count.load(std::memory_order_relaxed);

    // Sync barrier: wait for at least one audio block to be processed
    fx.send(osc_test::message("/sync", 200));
    OscReply syncR;
    fx.waitForReply("/synced", syncR);

    uint32_t after = metrics->process_count.load(std::memory_order_relaxed);

    CHECK(after > before);
}

TEST_CASE("messages_processed increases after sending /status", "[metrics]") {
    EngineFixture fx;
    auto* metrics = reinterpret_cast<PerformanceMetrics*>(ring_buffer_storage + METRICS_START);

    uint32_t before = metrics->messages_processed.load(std::memory_order_relaxed);
    fx.send(osc_test::message("/status"));

    // Sync barrier: ensure /status has been processed before reading metric
    fx.send(osc_test::message("/sync", 201));
    OscReply syncR;
    fx.waitForReply("/synced", syncR);

    uint32_t after = metrics->messages_processed.load(std::memory_order_relaxed);

    CHECK(after > before);
}

TEST_CASE("process_count is non-zero after engine boot", "[metrics]") {
    EngineFixture fx;
    auto* metrics = reinterpret_cast<PerformanceMetrics*>(ring_buffer_storage + METRICS_START);

    // EngineFixture pumps during construction
    uint32_t count = metrics->process_count.load(std::memory_order_relaxed);
    CHECK(count > 0);
}

TEST_CASE("messages_processed is non-zero after engine boot", "[metrics]") {
    EngineFixture fx;
    auto* metrics = reinterpret_cast<PerformanceMetrics*>(ring_buffer_storage + METRICS_START);

    // EngineFixture sends /g_new during construction
    uint32_t count = metrics->messages_processed.load(std::memory_order_relaxed);
    CHECK(count > 0);
}

TEST_CASE("scheduler_queue_depth is readable", "[metrics]") {
    EngineFixture fx;
    auto* metrics = reinterpret_cast<PerformanceMetrics*>(ring_buffer_storage + METRICS_START);

    // Just verify reading the atomic does not crash
    uint32_t depth = metrics->scheduler_queue_depth.load(std::memory_order_relaxed);
    (void)depth;
    SUCCEED();
}

TEST_CASE("in_buffer_peak_bytes is non-zero after sending messages", "[metrics]") {
    EngineFixture fx;
    auto* metrics = reinterpret_cast<PerformanceMetrics*>(ring_buffer_storage + METRICS_START);

    fx.send(osc_test::message("/status"));

    uint32_t peak = metrics->in_buffer_peak_bytes.load(std::memory_order_relaxed);
    CHECK(peak > 0);
}

TEST_CASE("out_buffer_peak_bytes is non-zero after getting replies", "[metrics]") {
    EngineFixture fx;
    auto* metrics = reinterpret_cast<PerformanceMetrics*>(ring_buffer_storage + METRICS_START);

    fx.send(osc_test::message("/status"));
    OscReply r;
    REQUIRE(fx.waitForReply("/status.reply", r));

    uint32_t peak = metrics->out_buffer_peak_bytes.load(std::memory_order_relaxed);
    CHECK(peak > 0);
}

TEST_CASE("messages_dropped starts at 0 or very low", "[metrics]") {
    EngineFixture fx;
    auto* metrics = reinterpret_cast<PerformanceMetrics*>(ring_buffer_storage + METRICS_START);

    uint32_t dropped = metrics->messages_dropped.load(std::memory_order_relaxed);
    // In normal operation immediately after boot, drops should be 0 or very low
    CHECK(dropped <= 1);
}

TEST_CASE("Metrics are at correct offset", "[metrics]") {
    EngineFixture fx;
    auto* metrics = reinterpret_cast<PerformanceMetrics*>(ring_buffer_storage + METRICS_START);

    // After boot and pumping, process_count must be non-zero.
    // If the offset were wrong we would read garbage or zero.
    uint32_t count = metrics->process_count.load(std::memory_order_relaxed);
    CHECK(count > 0);

    // Verify the offset constant itself
    CHECK(METRICS_START == CONTROL_START + CONTROL_SIZE);
}

TEST_CASE("Multiple /status sends increases messages_processed proportionally", "[metrics]") {
    EngineFixture fx;
    auto* metrics = reinterpret_cast<PerformanceMetrics*>(ring_buffer_storage + METRICS_START);

    uint32_t before = metrics->messages_processed.load(std::memory_order_relaxed);

    constexpr int numSends = 5;
    for (int i = 0; i < numSends; ++i) {
        fx.send(osc_test::message("/status"));
    }

    // Sync barrier: ensure all /status messages have been processed
    fx.send(osc_test::message("/sync", 202));
    OscReply syncR;
    fx.waitForReply("/synced", syncR);

    uint32_t after = metrics->messages_processed.load(std::memory_order_relaxed);
    uint32_t delta = after - before;

    // Each /status is one message, so delta should be at least numSends
    CHECK(delta >= static_cast<uint32_t>(numSends));
}

TEST_CASE("Buffer usage metrics are within expected ranges", "[metrics]") {
    EngineFixture fx;
    auto* metrics = reinterpret_cast<PerformanceMetrics*>(ring_buffer_storage + METRICS_START);

    fx.send(osc_test::message("/status"));

    uint32_t inUsed    = metrics->in_buffer_used_bytes.load(std::memory_order_relaxed);
    uint32_t outUsed   = metrics->out_buffer_used_bytes.load(std::memory_order_relaxed);
    uint32_t debugUsed = metrics->debug_buffer_used_bytes.load(std::memory_order_relaxed);

    CHECK(inUsed    < IN_BUFFER_SIZE);
    CHECK(outUsed   < OUT_BUFFER_SIZE);
    CHECK(debugUsed < DEBUG_BUFFER_SIZE);
}

TEST_CASE("prescheduler_bypassed increments for FAR_FUTURE bundles", "[metrics]") {
    EngineFixture fx;
    auto* metrics = reinterpret_cast<PerformanceMetrics*>(ring_buffer_storage + METRICS_START);

    uint32_t before = metrics->prescheduler_bypassed.load(std::memory_order_relaxed);

    // Send a bundle with timetag far in the future (wall + 60s, well beyond 0.5s lookahead)
    double futureNTP = NTP_EPOCH_OFFSET
                     + static_cast<double>(juce::Time::currentTimeMillis()) * 0.001
                     + 60.0;
    fx.engine().sendBundle(futureNTP, { OscBuilder::message("/status") });

    uint32_t after = metrics->prescheduler_bypassed.load(std::memory_order_relaxed);
    CHECK(after > before);
}

TEST_CASE("wasm_errors is 0 in normal operation", "[metrics]") {
    EngineFixture fx;
    auto* metrics = reinterpret_cast<PerformanceMetrics*>(ring_buffer_storage + METRICS_START);

    // Send a few normal commands
    fx.send(osc_test::message("/status"));

    uint32_t errors = metrics->wasm_errors.load(std::memory_order_relaxed);
    CHECK(errors == 0);
}
