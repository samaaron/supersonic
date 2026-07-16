/*
 * test_scope_stream.cpp — shm_scope_stream protocol + sample clock.
 *
 * Unit-level: writer/reader round-trip, ring wrap, engine-frame anchoring and
 * gap healing (paused node groups skip blocks), copy_window clamping and
 * zero-fill. Engine-level: the headless driver publishes the sample clock
 * every block; readers see a consistent, advancing (frames ↔ NTP) mapping.
 */
#include <catch2/catch_test_macros.hpp>
#include "EngineFixture.h"
#include "OscTestUtils.h"

#include "src/synth/common/shm_scope_stream.hpp"
#include "src/synth/common/server_shm.hpp"

#include <cmath>
#include <memory>
#include <vector>

using detail_server_shm::server_shared_memory_client;
using detail_server_shm::sample_clock_view;

namespace {

std::unique_ptr<shm_scope_stream> makeSlot() {
    auto slot = std::make_unique<shm_scope_stream>();
    memset(static_cast<void*>(slot.get()), 0, sizeof(shm_scope_stream));
    return slot;
}

// One stereo block whose samples encode their absolute frame index, so
// windows read back from any cursor position self-identify.
void writeIndexedBlock(shm_scope_stream_writer& w, uint64_t engineFrames,
                       uint32_t n, uint64_t indexBase) {
    std::vector<float> left(n), right(n);
    for (uint32_t i = 0; i < n; ++i) {
        left[i] = static_cast<float>(indexBase + i);
        right[i] = -static_cast<float>(indexBase + i);
    }
    const float* ch[2] = { left.data(), right.data() };
    w.write(ch, n, engineFrames);
}

}  // namespace

TEST_CASE("scope-stream: writer/reader round-trip with interleaved windows",
          "[scope][stream]") {
    auto slot = makeSlot();
    shm_scope_stream_writer w(slot.get());
    w.activate(2);
    shm_scope_stream_reader r(slot.get());

    REQUIRE(r.valid());
    CHECK(r.channels() == 2);
    CHECK(r.capacity_frames() == SHM_SCOPE_RING_FRAMES);
    CHECK(r.write_position() == 0);

    writeIndexedBlock(w, 5000, 128, 0);
    writeIndexedBlock(w, 5128, 128, 128);
    CHECK(r.write_position() == 256);
    CHECK(r.base_engine_frames() == 5000);

    std::vector<float> out(64 * 2, -1.0f);
    const uint32_t real = r.copy_window(256, 64, out.data());
    CHECK(real == 64);
    // Window ends at cursor 256 → frames 192..255, interleaved L/R.
    for (uint32_t i = 0; i < 64; ++i) {
        CHECK(out[i * 2] == static_cast<float>(192 + i));
        CHECK(out[i * 2 + 1] == -static_cast<float>(192 + i));
    }
}

TEST_CASE("scope-stream: short history zero-fills the window lead-in",
          "[scope][stream]") {
    auto slot = makeSlot();
    shm_scope_stream_writer w(slot.get());
    w.activate(2);
    shm_scope_stream_reader r(slot.get());

    writeIndexedBlock(w, 100, 32, 0);
    std::vector<float> out(128 * 2, -1.0f);
    const uint32_t real = r.copy_window(32, 128, out.data());
    CHECK(real == 32);
    // 96 lead-in frames zero-filled, then the 32 real frames.
    for (uint32_t i = 0; i < 96; ++i) {
        CHECK(out[i * 2] == 0.0f);
        CHECK(out[i * 2 + 1] == 0.0f);
    }
    CHECK(out[96 * 2] == 0.0f);       // frame index 0
    CHECK(out[127 * 2] == 31.0f);     // frame index 31
}

TEST_CASE("scope-stream: ring wrap preserves window continuity",
          "[scope][stream]") {
    auto slot = makeSlot();
    shm_scope_stream_writer w(slot.get());
    w.activate(2);
    shm_scope_stream_reader r(slot.get());

    // Write ~1.5 rings in 512-frame blocks.
    const uint32_t cap = SHM_SCOPE_RING_FRAMES;
    const uint64_t total = cap + cap / 2;
    uint64_t engine = 0;
    for (uint64_t at = 0; at < total; at += 512) {
        writeIndexedBlock(w, 77777 + engine, 512, at);
        engine += 512;
    }
    CHECK(r.write_position() == total);

    // A window that straddles the wrap point must still be contiguous.
    std::vector<float> out(1024 * 2, -1.0f);
    const uint32_t real = r.copy_window(total, 1024, out.data());
    CHECK(real == 1024);
    for (uint32_t i = 0; i < 1024; ++i)
        CHECK(out[i * 2] == static_cast<float>(total - 1024 + i));
}

TEST_CASE("scope-stream: engine-frame gaps heal the cursor mapping",
          "[scope][stream]") {
    auto slot = makeSlot();
    shm_scope_stream_writer w(slot.get());
    w.activate(2);
    shm_scope_stream_reader r(slot.get());

    writeIndexedBlock(w, 1000, 128, 0);
    // A paused node group skips ten blocks: the next write arrives with an
    // engine position 1280 frames on. The cursor must jump so cursor↔engine
    // stays exact (slot cursor == engine - base).
    writeIndexedBlock(w, 1000 + 128 + 1280, 128, 128);
    CHECK(r.base_engine_frames() == 1000);
    CHECK(r.write_position() == 128 + 1280 + 128);
}

TEST_CASE("scope-stream: copy_window clamps a cursor beyond the writer",
          "[scope][stream]") {
    auto slot = makeSlot();
    shm_scope_stream_writer w(slot.get());
    w.activate(2);
    shm_scope_stream_reader r(slot.get());

    writeIndexedBlock(w, 0, 256, 0);
    std::vector<float> out(64 * 2, -1.0f);
    // Ask for a window ending far past what was written: clamps to writer.
    const uint32_t real = r.copy_window(1u << 20, 64, out.data());
    CHECK(real == 64);
    CHECK(out[63 * 2] == 255.0f);
}

TEST_CASE("scope-stream: audible_end windows streams on the sample clock",
          "[scope][stream][sampleclock]") {
    auto slot = makeSlot();
    shm_scope_stream_writer w(slot.get());
    w.activate(2);
    shm_scope_stream_reader r(slot.get());
    writeIndexedBlock(w, 10000, 1024, 0);  // base=10000, writer=1024

    sample_clock_view v;
    v.valid = true;
    v.sample_rate = 48000;
    v.engine_frames = 10512;
    v.dac_ntp = 100.0;  // engine frame 10512 audible at t=100

    CHECK(r.write_position() == 1024);
    CHECK(v.audible_end(r, 100.0) == 512);   // mid-stream
    CHECK(v.audible_end(r, 200.0) == 1024);  // future: clamps to writer
    CHECK(v.audible_end(r, 99.0) == 0);      // pre-anchor: silence, never unheard audio
    CHECK(sample_clock_view().audible_end(r, 100.0) == 1024);  // no clock: writer
}

TEST_CASE("scope-stream: headless engine publishes an advancing sample clock",
          "[scope][stream][sampleclock]") {
    SupersonicEngine::Config cfg;
    cfg.sampleRate        = 48000;
    cfg.bufferSize        = 128;
    cfg.udpPort           = 57317;  // non-zero enables shared memory
    cfg.headless          = true;
    cfg.numOutputChannels = 2;
    cfg.numInputChannels  = 0;
    EngineFixture fix(cfg);

    OscReply reply;
    fix.send(osc_test::message("/status"));
    REQUIRE(fix.waitForReply("/status.reply", reply));
    REQUIRE(fix.waitForBlocks(20, 3000));

    server_shared_memory_client client(cfg.udpPort);
    const sample_clock_view a = client.get_sample_clock();
    REQUIRE(a.valid);
    CHECK(a.sample_rate == 48000);
    CHECK(a.output_latency_frames == 0);  // headless: no DAC

    REQUIRE(fix.waitForBlocks(40, 3000));
    const sample_clock_view b = client.get_sample_clock();
    REQUIRE(b.valid);
    CHECK(b.engine_frames > a.engine_frames);
    // NTP advances with the sample clock: the two snapshots must agree to
    // within a generous scheduling tolerance (headless ticks are timed).
    const double dtFrames = double(b.engine_frames - a.engine_frames) / 48000.0;
    const double dtNtp = b.dac_ntp - a.dac_ntp;
    CHECK(std::abs(dtNtp - dtFrames) < 0.25);

    // visible_frames at the publish instant is close to the published frame.
    const double vis = b.visible_frames(b.dac_ntp);
    CHECK(std::abs(vis - double(b.engine_frames)) < 1.0);
}
