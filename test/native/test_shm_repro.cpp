#include "EngineFixture.h"

TEST_CASE("Engine boots with udpPort > 0 (cross-process shm)", "[shm-repro]") {
    ClockworkEngine::Config cfg;
    cfg.sampleRate       = 48000;
    cfg.bufferSize       = 128;
    cfg.udpPort          = 30099;  // non-zero → creates POSIX shm
    setGuestOption(cfg, "numBuffers", 1024);
    setGuestOption(cfg, "maxNodes", 1024);
    setGuestOption(cfg, "maxGraphDefs", 512);
    setGuestOption(cfg, "maxWireBufs", 64);
    cfg.headless         = true;

    EngineFixture fx(cfg);

    fx.send(osc_test::message("/status"));
    OscReply r;
    REQUIRE(fx.waitForReply("/status.reply", r));
    auto p = r.parsed();
    REQUIRE(p.argCount() >= 5);
}
