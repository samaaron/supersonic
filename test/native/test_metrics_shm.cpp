/*
 * test_metrics_shm.cpp — Native metrics observable via the public POSIX shm
 * segment.
 *
 * Verifies the redirect added in src/native/ClockworkEngine.cpp:
 *  - When the engine creates a SuperSonic_<port> public shm segment, the
 *    metrics struct lives inside that segment instead of in the in-band
 *    slot inside ring_buffer_storage.
 *  - An external observer can mmap the segment via shm_segment_client
 *    and read the same metrics struct that engine writers update.
 *
 * EngineFixture defaults to udpPort = 0 which skips shm creation; this
 * spec uses a non-zero port + headless so the redirect path exercises.
 */
#include <catch2/catch_test_macros.hpp>
#include "EngineFixture.h"
#include "OscBuilder.h"
#include "OscTestUtils.h"
#include "shm_segment.hpp"
#include "shared_memory.h"

extern "C" uint8_t ring_buffer_storage[];

namespace {
ClockworkEngine::Config metricsShmConfig(unsigned port) {
    ClockworkEngine::Config cfg;
    cfg.sampleRate    = 48000;
    cfg.bufferSize    = 128;
    cfg.udpPort       = port;  // non-zero enables the public shm segment
    cfg.numBuffers    = 256;
    cfg.maxNodes      = 256;
    cfg.maxGraphDefs  = 64;
    cfg.maxWireBufs   = 32;
    cfg.headless      = true;
    return cfg;
}
}  // namespace

TEST_CASE("metrics-shm: engine.getMetrics() points into the public segment when udpPort > 0",
          "[metrics][shm]") {
    constexpr unsigned kPort = 57211;
    EngineFixture fx(metricsShmConfig(kPort));

    const PerformanceMetrics* m = &fx.engine().getMetrics();
    auto* base = reinterpret_cast<const uint8_t*>(m);
    auto* ring = ring_buffer_storage;

    // The metrics pointer must NOT be inside ring_buffer_storage now —
    // it lives in the public shm segment.
    bool insideRing = (base >= ring) && (base < ring + TOTAL_BUFFER_SIZE);
    CHECK_FALSE(insideRing);
}

TEST_CASE("metrics-shm: external client sees the same metrics struct",
          "[metrics][shm]") {
    constexpr unsigned kPort = 57212;
    EngineFixture fx(metricsShmConfig(kPort));

    // Drive at least one OSC message so a metric we can verify increments.
    fx.send(osc_test::message("/status"));
    fx.send(osc_test::message("/sync", 0));
    OscReply r;
    REQUIRE(fx.waitForReply("/synced", r));

    // Open the public segment as a separate client: a duplicate of the
    // engine's handle, mapped again. A second mapping gets a fresh virtual
    // address even within one process, so the client pointer differs from the
    // engine pointer numerically — they refer to the same physical pages.
    shm_segment_client client(detail_shm_segment::shm_dup_handle(fx.engine().shmNativeHandle()));
    PerformanceMetrics* externalMetrics = client.get_metrics();
    REQUIRE(externalMetrics != nullptr);

    // The client must observe the activity that just happened.
    CHECK(externalMetrics->messages_processed.load() > 0);
    CHECK(externalMetrics->osc_out_messages_sent.load() > 0);
}

TEST_CASE("metrics-shm: increments via engine writes are visible through client",
          "[metrics][shm]") {
    constexpr unsigned kPort = 57213;
    EngineFixture fx(metricsShmConfig(kPort));
    shm_segment_client client(detail_shm_segment::shm_dup_handle(fx.engine().shmNativeHandle()));
    PerformanceMetrics* externalMetrics = client.get_metrics();
    REQUIRE(externalMetrics != nullptr);

    uint32_t before = externalMetrics->messages_processed.load();

    constexpr int N = 5;
    for (int i = 0; i < N; ++i) fx.send(osc_test::message("/status"));
    fx.send(osc_test::message("/sync", 1));
    OscReply r;
    REQUIRE(fx.waitForReply("/synced", r));

    uint32_t after = externalMetrics->messages_processed.load();
    CHECK(after - before >= static_cast<uint32_t>(N));
}
