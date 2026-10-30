/*
 * test_scheduler_overflow.cpp — timed bundles past the scheduler's capacity:
 * the surplus is dropped and counted, the bundles that fit play at their
 * time, and command intake keeps working throughout.
 *
 * The test renders every block itself (the fixture's manual pump) on a
 * freewheel clock from a fixed origin, so nothing comes due while the burst
 * is being sent and the accounting is exact: every bundle sent is either held
 * or dropped.
 */
#include "EngineFixture.h"
#include "ClockworkProcessor.h"   // get_audio_output_bus / get_audio_buffer_samples
#include "shared_memory.h"        // PerformanceMetrics

#include <cmath>
#include <cstring>
#include <string>
#include <vector>

namespace {

constexpr double kOriginNtp = 3'900'000'000.0;
constexpr double kRate      = 48000.0;

ClockworkEngine::Config config() {
    auto cfg = EngineFixture::defaultConfig();
    cfg.manualAudioPump    = true;
    cfg.freewheelClock     = true;
    cfg.freewheelOriginNtp = kOriginNtp;
    setGuestOption(cfg, "maxNodes", 4096);
    return cfg;
}

// The time the next block is heard at, on the engine's freewheel clock.
double engineNow(EngineFixture& fx) {
    const uint32_t blocks = fx.engine().processor().processCount.load(std::memory_order_acquire);
    return kOriginNtp + double(blocks) * get_audio_buffer_samples() / kRate;
}

uint32_t blocksFor(double seconds) {
    return static_cast<uint32_t>(std::ceil(seconds * kRate / get_audio_buffer_samples()));
}

// "#bundle", the timetag, then one element behind its size.
std::vector<uint8_t> bundle(double ntp, const osc_test::Packet& msg) {
    const double whole = std::floor(ntp);
    const uint64_t tag = (static_cast<uint64_t>(whole) << 32)
                       | static_cast<uint64_t>((ntp - whole) * 4294967296.0);
    std::vector<uint8_t> out(16 + 4 + msg.size());
    std::memcpy(out.data(), "#bundle\0", 8);
    for (int i = 0; i < 8; ++i) out[8 + i] = static_cast<uint8_t>(tag >> (56 - 8 * i));
    const uint32_t n = msg.size();
    for (int i = 0; i < 4; ++i) out[16 + i] = static_cast<uint8_t>(n >> (24 - 8 * i));
    std::memcpy(out.data() + 20, msg.ptr(), n);
    return out;
}

// A quiet beep; `pad` is an unknown control, dropped by the synth, whose only
// job is to make each bundle big enough that a few hundred exhaust the pool.
osc_test::Packet beep(int32_t node, int32_t note, size_t padBytes) {
    osc_test::Builder b;
    auto& m = b.begin("/s_new");
    m << "sonic-pi-beep" << node << int32_t(0) << int32_t(0)
      << "note" << float(note) << "amp" << 0.05f << "release" << 0.2f;
    if (padBytes) {
        const std::string pad(padBytes, 'x');
        m << "pad" << pad.c_str();
    }
    return b.end();
}

// The loudest left-channel sample over the next `blocks`, read off the output
// bus after each block renders.
float peakOver(EngineFixture& fx, uint32_t blocks) {
    float peak = 0;
    const int frames = get_audio_buffer_samples();
    for (uint32_t i = 0; i < blocks; ++i) {
        fx.pumpBlock();
        const auto* bus = reinterpret_cast<const float*>(get_audio_output_bus());
        for (int f = 0; f < frames; ++f) peak = std::max(peak, std::fabs(bus[f]));
    }
    return peak;
}

} // namespace

TEST_CASE("scheduler overflow: the surplus is dropped and counted, what fits plays at its time, "
          "and intake keeps working", "[scheduler][overflow]") {
    EngineFixture fx(config());
    REQUIRE(fx.loadSynthDef("sonic-pi-beep"));
    const PerformanceMetrics& m = fx.engine().getMetrics();
    const uint32_t poolBytes = m.scheduler_pool_bytes.load();
    REQUIRE(poolBytes > 0);

    constexpr size_t   kPad      = 4096;
    constexpr uint32_t kOverflow = 50;
    const uint32_t total = (poolBytes + kPad - 1) / kPad + kOverflow;
    const uint32_t droppedBefore = m.scheduler_queue_dropped.load();

    // Due 10 s of engine time from now. Sending renders a block per 32
    // bundles, so the burst takes a few dozen blocks: nothing comes due.
    const double due = engineNow(fx) + 10.0;
    for (uint32_t i = 0; i < total; ++i) {
        const auto b = bundle(due + i * 0.0001, beep(static_cast<int32_t>(50000 + i), 60 + int32_t(i % 24), kPad));
        fx.send(b.data(), static_cast<uint32_t>(b.size()));
        if ((i & 31) == 31) fx.pumpBlock();
    }
    fx.pumpBlock(2);
    REQUIRE(engineNow(fx) < due);

    const uint32_t dropped = m.scheduler_queue_dropped.load() - droppedBefore;
    const uint32_t held    = m.scheduler_queue_depth.load();
    INFO(total << " sent into a pool of " << poolBytes << " bytes: " << held << " held, " << dropped << " dropped");
    CHECK(dropped > 0);
    CHECK(held + dropped == total);
    CHECK(m.messages_dropped.load() == 0);   // the ingress took every one

    // Intake still works: an immediate beep plays now.
    {
        osc_test::Builder b;
        b.begin("/s_new") << "sonic-pi-beep" << int32_t(99999) << int32_t(0) << int32_t(0)
                          << "amp" << 0.5f << "release" << 0.1f;
        fx.send(b.end());
        CHECK(peakOver(fx, blocksFor(0.15)) > 0.01f);
    }

    // Silence until the bundles are due, then the ones that fit play.
    fx.pumpBlock(blocksFor(due - engineNow(fx)) - 1);
    CHECK(peakOver(fx, blocksFor(0.15)) > 0.001f);
    CHECK(m.scheduler_queue_depth.load() == 0);
}
