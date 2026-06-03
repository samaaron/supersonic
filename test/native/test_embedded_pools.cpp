/*
 * test_embedded_pools.cpp — regression guard: scsynth core stays memory-safe
 * when synths are instantiated into a World sized with the embedded firmware's
 * tight pools.
 *
 * The ESP32 firmware once hard-faulted in the I2S DMA ISR when synths were
 * created under tight pools, which looked like a silent buffer overrun in the
 * engine. Investigation showed otherwise: the wire-buffer path is bounds-checked
 * (a too-wide def throws cleanly because EngineCore_New starts the world, so
 * mRunning is set), node/graphdef/bus pools all fail cleanly, and this very test
 * — driving the real supersaw/reverb/pluck through tight pools under
 * AddressSanitizer — comes back clean. So the crash was firmware-side, not a
 * core overflow. This test locks that conclusion in: built in the CI
 * sanitize-asan-ubsan job, any future regression that lets a synth overrun a
 * World pool gets named at its exact file:line.
 */
#include "EngineFixture.h"
#include "OscTestUtils.h"

#include <catch2/catch_test_macros.hpp>

namespace {

// The ESP32 firmware's original (tight) WorldOptions.
SupersonicEngine::Config embeddedConfig() {
    SupersonicEngine::Config cfg;
    cfg.sampleRate            = 48000;
    cfg.bufferSize            = 64;
    cfg.maxNodes              = 256;
    cfg.numBuffers            = 64;
    cfg.maxGraphDefs          = 128;
    cfg.maxWireBufs           = 64;
    cfg.numAudioBusChannels   = 64;
    cfg.numControlBusChannels = 256;
    cfg.numOutputChannels     = 2;
    cfg.realTimeMemorySize    = 128;   // KB — the tight RT pool
    cfg.headless              = true;
    return cfg;
}

void newSynth(EngineFixture& fx, const char* def, int32_t node) {
    osc_test::Builder b;
    auto& s = b.begin("/s_new");
    s << def << node << (int32_t)0 << (int32_t)0;  // addToHead, root group
    fx.send(b.end());
}

} // namespace

TEST_CASE("Core stays memory-safe instantiating synths under embedded-tight pools",
          "[embedded]") {
    EngineFixture fx(embeddedConfig());

    // Soft loads: a def needing > maxWireBufs throws cleanly at /d_recv (data,
    // not a fault); keep going so we still reach instantiation + render.
    fx.loadSynthDef("sonic-pi-supersaw");
    fx.loadSynthDef("sonic-pi-fx_reverb");     // delay-line heavy — RT-pool hog
    fx.loadSynthDef("sonic-pi-pluck");

    newSynth(fx, "sonic-pi-fx_reverb", 2000);
    newSynth(fx, "sonic-pi-supersaw", 1000);
    newSynth(fx, "sonic-pi-pluck", 3000);
    // pile on to push the 128 KB RT pool toward exhaustion (must fail cleanly)
    for (int i = 0; i < 16; ++i)
        newSynth(fx, "sonic-pi-pluck", 3001 + i);

    // Render: first-block UGen constructors (delay-line allocs etc.) run here.
    fx.waitForBlocks(96);
    SUCCEED();
}
