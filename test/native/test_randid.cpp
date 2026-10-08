/*
 * test_randid.cpp — RandID across blocks (upstream #6159, commit 91cc120).
 *
 * Two RandIDs in one SynthDef, each switching to its own generator and
 * seeding it the same: drawn alike, the two noises are identical. RandID
 * switches on every control block now, so that holds on every block and not
 * only the first. The probe, and why it is built as it is, is in
 * test/synthdefs/compile_randid_synthdefs.scd.
 */
#include "EngineFixture.h"
#include "ClockworkProcessor.h"   // get_audio_output_bus
#include <algorithm>
#include <cmath>

TEST_CASE("RandID: two in one SynthDef draw from two generators on every block",
          "[randid][upstream]") {
    // This thread renders every block, so the bus is whole when it is read.
    auto cfg = EngineFixture::defaultConfig();
    cfg.manualAudioPump = true;
    EngineFixture fx(cfg);
    REQUIRE(fx.loadSynthDef("randid_two_ids_probe"));
    {
        osc_test::Builder b;
        auto& s = b.begin("/s_new");
        s << "randid_two_ids_probe" << (int32_t)2000 << (int32_t)0 << (int32_t)1;
        fx.send(b.end());
    }
    OscReply synced;
    fx.send(osc_test::message("/sync", 1));
    REQUIRE(fx.waitForReply("/synced", synced));

    // The output bus is channel-major, one block per channel.
    const uint32_t block = static_cast<uint32_t>(fx.engine().processor().bufferLength());
    float differ = 0.0f, drawn = 0.0f;
    for (int i = 0; i < 8; ++i) {
        fx.pumpBlock();
        const auto* bus = reinterpret_cast<const float*>(get_audio_output_bus());
        REQUIRE(bus != nullptr);
        for (uint32_t k = 0; k < block; ++k) {
            differ = std::max(differ, std::fabs(bus[k]));          // channel 0: |x - y|
            drawn  = std::max(drawn,  std::fabs(bus[block + k]));  // channel 1: x
        }
    }
    REQUIRE(drawn > 0.1f);   // there is noise to compare
    CHECK(differ == 0.0f);
    fx.send(osc_test::message("/n_free", 2000));
}
