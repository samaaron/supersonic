/*
 * test_in_ring_drain.cpp — The audio-thread IN-ring drain must survive a
 * malformed frame without hanging or scanning the whole buffer.
 *
 * The drain in process_audio() consumes OSC frames the transport (or, on
 * native, a cross-process writer to the public shm segment) places in the IN
 * ring. The cursors and frame headers are therefore NOT trusted. These tests
 * inject frames the writer would never produce and assert the drain:
 *   - terminates (a length < header size previously underflowed the payload
 *     size and left the tail un-advanced → an infinite loop on the audio
 *     thread; if that regresses, process_audio() never returns and the test
 *     times out),
 *   - resyncs the tail to head (dropping the suspect region) rather than
 *     walking it one byte at a time, and
 *   - counts the drop.
 *
 * Manual-pump + udpPort 0: this thread is the sole process_audio() caller and
 * the arena is the in-process ring_buffer_storage, so writing raw frames into
 * it races nothing.
 */
#include <catch2/catch_test_macros.hpp>
#include "EngineFixture.h"
#include "src/shared_memory.h"

#include <cstdint>
#include <cstring>

extern "C" uint8_t ring_buffer_storage[];
extern "C" bool process_audio(double current_time, uint32_t active_output_channels,
                              uint32_t active_input_channels);

namespace {

SupersonicEngine::Config drainConfig() {
    auto cfg = EngineFixture::defaultConfig();
    cfg.manualAudioPump = true;  // test thread owns process_audio()
    return cfg;
}

ControlPointers* control() {
    return reinterpret_cast<ControlPointers*>(ring_buffer_storage + CONTROL_START);
}

// One process_audio() block on the test thread (channel counts match the
// fixture's 2-in/2-out world). Returning at all is itself the liveness check.
void pump() {
    process_audio(4'000'000'000.0 /*arbitrary NTP*/, 2, 2);
}

// Drain whatever boot traffic is queued, then park both cursors at 0 so the
// injected frame sits at a known, non-wrapping offset.
void quiesceAndReset(EngineFixture& fx) {
    for (int i = 0; i < 8; ++i) pump();
    control()->in_head.store(0, std::memory_order_release);
    control()->in_tail.store(0, std::memory_order_release);
    (void)fx;
}

// Write a 16-byte frame header at IN offset 0 and publish head one header past
// it, so the drain sees exactly one frame available.
void injectHeader(uint32_t magic, uint32_t length) {
    uint32_t hdr[4] = { magic, length, /*sequence*/ 0, /*sourceId*/ 0 };
    std::memcpy(ring_buffer_storage + IN_BUFFER_START, hdr, sizeof(hdr));
    control()->in_tail.store(0, std::memory_order_release);
    control()->in_head.store(static_cast<int32_t>(sizeof(Message)),
                             std::memory_order_release);
}

} // namespace

TEST_CASE("in-ring-drain: zero-length frame does not hang and is dropped",
          "[ingress][ring][security]") {
    EngineFixture fx(drainConfig());
    quiesceAndReset(fx);

    const uint32_t droppedBefore =
        control() ? fx.engine().getMetrics().messages_dropped.load() : 0;

    // length == 0 < sizeof(Message): the pre-fix payload-size computation
    // underflowed and the drop path advanced the tail by length (0), spinning
    // forever. If pump() returns, the lower-bound guard fired.
    injectHeader(MESSAGE_MAGIC, 0);
    pump();

    auto* ctrl = control();
    CHECK(ctrl->in_tail.load() == ctrl->in_head.load());  // resynced, not stuck
    CHECK(fx.engine().getMetrics().messages_dropped.load() == droppedBefore + 1);
}

TEST_CASE("in-ring-drain: over-long frame is dropped and resynced",
          "[ingress][ring][security]") {
    EngineFixture fx(drainConfig());
    quiesceAndReset(fx);

    const uint32_t droppedBefore = fx.engine().getMetrics().messages_dropped.load();

    // length beyond the buffer: must be rejected, not used to index the ring.
    injectHeader(MESSAGE_MAGIC, IN_BUFFER_SIZE + 64);
    pump();

    auto* ctrl = control();
    CHECK(ctrl->in_tail.load() == ctrl->in_head.load());
    CHECK(fx.engine().getMetrics().messages_dropped.load() == droppedBefore + 1);
}

TEST_CASE("in-ring-drain: bad magic resyncs to head instead of byte-walking",
          "[ingress][ring][security]") {
    EngineFixture fx(drainConfig());
    quiesceAndReset(fx);

    const uint32_t droppedBefore = fx.engine().getMetrics().messages_dropped.load();

    // A garbage header followed by valid bytes up to head: the drain must jump
    // the tail straight to head (the whole region after corruption is suspect),
    // not advance one byte per iteration scanning for the next magic.
    injectHeader(0xABADCAFE, 64);
    control()->in_head.store(static_cast<int32_t>(IN_BUFFER_SIZE / 2),
                             std::memory_order_release);
    pump();

    auto* ctrl = control();
    CHECK(ctrl->in_tail.load() == ctrl->in_head.load());
    CHECK(fx.engine().getMetrics().messages_dropped.load() == droppedBefore + 1);
}

TEST_CASE("in-ring-drain: out-of-range cursor is clamped, drain skipped safely",
          "[ingress][ring][security]") {
    EngineFixture fx(drainConfig());
    quiesceAndReset(fx);

    // A tail outside [0, IN_BUFFER_SIZE) would index outside the IN region.
    // The drain must repair it (resync tail to head) and not dereference it.
    control()->in_head.store(0, std::memory_order_release);
    control()->in_tail.store(0x7FFFFFFF, std::memory_order_release);
    pump();  // must return without an OOB read

    CHECK(static_cast<uint32_t>(control()->in_tail.load()) < IN_BUFFER_SIZE);
}
