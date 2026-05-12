#include "EngineFixture.h"

TEST_CASE("Engine boots with udpPort > 0 (cross-process shm)", "[shm-repro]") {
    SupersonicEngine::Config cfg;
    cfg.sampleRate       = 48000;
    cfg.bufferSize       = 128;
    cfg.udpPort          = 30099;  // non-zero → creates POSIX shm
    cfg.numBuffers       = 1024;
    cfg.maxNodes         = 1024;
    cfg.maxGraphDefs     = 512;
    cfg.maxWireBufs      = 64;
    cfg.headless         = true;

    EngineFixture fx(cfg);

    fx.send(osc_test::message("/status"));
    OscReply r;
    REQUIRE(fx.waitForReply("/status.reply", r));
    auto p = r.parsed();
    REQUIRE(p.argCount() >= 5);
}
