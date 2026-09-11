// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Sam Aaron
/*
 * test_front_recording.cpp — session recording is the client's.
 *
 * The engine opens no files. The master mix leaves through clockwork's OUT
 * tap, written at the device edge from boot with nobody holding anything.
 * Recording a session is reading that slot from its live position:
 * SuperSonic's front answers /clockwork/record/start and
 * /clockwork/record/stop by pulling the slot on a thread of its own into
 * the writer API, and replies in the shape the engine used to —
 * record/start.reply <ok> <path|error>.
 */
#include "EngineFixture.h"
#include "OscTestUtils.h"
#include "SuperSonicFront.h"
#include "IOscTransport.h"
#include "clockwork_audio_file.h"
#include "clockwork_client.h"
#include "shm_audio_buffer.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include "TestPid.h"

namespace {

constexpr uint32_t kAsker = 0x4EC0;

struct RecordingSink final : IOscTransport {
    struct Sent { uint32_t token; OscReply reply; };
    std::mutex mu;
    std::vector<Sent> sent;
    bool send(uint32_t token, const uint8_t* d, uint32_t n, bool) override {
        std::lock_guard<std::mutex> lock(mu);
        sent.push_back({ token, OscReply{ osc_test::parseAddress(d, n), std::vector<uint8_t>(d, d + n) } });
        return true;
    }
    void broadcastNotify(const uint8_t*, uint32_t) override {}
    void broadcastLink(const uint8_t*, uint32_t) override {}
    bool hasNotifySubscribers() const override { return false; }
    bool subscribeNotify(uint32_t) override { return false; }
    void subscribeNotifyPort(int) override {}
    void unsubscribeNotify(uint32_t) override {}
    void clearNotify() override {}
    bool subscribeLink(uint32_t) override { return false; }
    void unsubscribeLink(uint32_t) override {}
    void broadcastMidi(const uint8_t*, uint32_t) override {}
    bool subscribeMidi(uint32_t) override { return false; }
    void unsubscribeMidi(uint32_t) override {}
    void broadcastGamepad(const uint8_t*, uint32_t) override {}
    bool subscribeGamepad(uint32_t) override { return false; }
    void unsubscribeGamepad(uint32_t) override {}
    bool waitFor(const char* addr, uint32_t token, OscReply* out, int timeoutMs = 3000) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
        while (std::chrono::steady_clock::now() < deadline) {
            {
                std::lock_guard<std::mutex> lock(mu);
                for (const auto& s : sent)
                    if (s.token == token && s.reply.address == addr) { if (out) *out = s.reply; return true; }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        return false;
    }
};

struct Rig {
    EngineFixture    fx;
    RecordingSink    sink;
    SuperSonicFront  front;
    shm_audio_buffer* slot0 = nullptr;
    Rig() : front(fx.engine(), &sink) {
        fx.engine().onReplyRouted = [this](uint32_t origin, uint32_t, const uint8_t* d, uint32_t n) {
            if (!front.egress(origin, d, n)) sink.send(origin, d, n, false);
        };
        ClockworkRegion taps {};
        REQUIRE(clockwork_client_region(fx.engine().egressClient(), CLOCKWORK_REGION_AUDIO_TAPS, &taps) == CLOCKWORK_OK);
        slot0 = static_cast<shm_audio_buffer*>(taps.base);
    }
    ~Rig() { fx.engine().onReplyRouted = nullptr; }
    bool ingress(const osc_test::Packet& p) { return front.ingress(p.ptr(), p.size(), kAsker); }
    OscReply expect(const char* addr) {
        OscReply r;
        REQUIRE(sink.waitFor(addr, kAsker, &r));
        return r;
    }
};

std::string tempWav(const char* stem, const char* ext = "wav") {
    return (std::filesystem::temp_directory_path() / (std::string(stem) + "-" + std::to_string(testPid()) + "." + ext)).string();
}

struct Decoded { ClockworkAudioInfo info {}; std::vector<float> samples; };
Decoded decode(const std::string& path) {
    Decoded d;
    d.info.struct_bytes = sizeof d.info;
    float* out = nullptr;
    REQUIRE(clockwork_audio_decode_file(path.c_str(), &d.info, &out) == CLOCKWORK_OK);
    d.samples.assign(out, out + d.info.frames * d.info.channels);
    clockwork_audio_free(out);
    return d;
}

} // namespace

TEST_CASE("recording: /clockwork/record/start and /stop write clockwork's OUT tap to a file, on the client's thread",
          "[front][recording]") {
    Rig rig;
    const std::string path = tempWav("front-record");

    // The tap has been flowing since boot; the recorder records what
    // arrives from now on.
    osc_test::Builder b;
    b.begin("/clockwork/record/start") << path.c_str() << "wav" << int32_t{32};
    REQUIRE(rig.ingress(b.end()));
    const auto started = rig.expect("/clockwork/record/start.reply");
    CHECK(started.parsed().argInt(0) == 1);
    CHECK(started.parsed().argString(1) == path);
    CHECK(rig.front.recording());

    // A second start while recording is refused, as it was.
    rig.sink.sent.clear();
    {
        osc_test::Builder b2;
        b2.begin("/clockwork/record/start") << tempWav("other").c_str() << "wav" << int32_t{16};
        REQUIRE(rig.ingress(b2.end()));
    }
    const auto again = rig.expect("/clockwork/record/start.reply");
    CHECK(again.parsed().argInt(0) == 0);
    CHECK(again.parsed().argString(1).find("already recording") != std::string::npos);

    // Clockwork feeds the tap block by block.
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    CHECK(rig.slot0->enabled.load() == 1);

    rig.sink.sent.clear();
    REQUIRE(rig.ingress(osc_test::message("/clockwork/record/stop")));
    const auto stopped = rig.expect("/clockwork/record/stop.reply");
    CHECK(stopped.parsed().argInt(0) == 1);
    CHECK(stopped.parsed().argString(1) == path);
    CHECK_FALSE(rig.front.recording());

    // The file is complete, at the tap's geometry, with the time's worth of
    // frames (silence: nothing is playing) and none lost.
    const Decoded d = decode(path);
    CHECK(d.info.channels == 2);
    CHECK(d.info.sample_rate == 48000);
    CHECK(d.info.frames > 48000 * 0.15);
    CHECK(d.info.frames < 48000 * 0.7);
    CHECK(rig.front.recordingFramesLost() == 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(30));   // the release reaches the guest next block
    CHECK(rig.slot0->enabled.load() == 1);   // the tap flows on; recordings come and go
    std::remove(path.c_str());

    // Stop with nothing recording is refused, as it was.
    rig.sink.sent.clear();
    REQUIRE(rig.ingress(osc_test::message("/clockwork/record/stop")));
    const auto nothing = rig.expect("/clockwork/record/stop.reply");
    CHECK(nothing.parsed().argInt(0) == 0);
    CHECK(nothing.parsed().argString(1).find("not recording") != std::string::npos);
}

TEST_CASE("recording: a format the codecs cannot write is refused at once, and no file is touched",
          "[front][recording]") {
    Rig rig;
    const std::string path = tempWav("front-record-bad", "mp3");
    osc_test::Builder b;
    b.begin("/clockwork/record/start") << path.c_str() << "mp3" << int32_t{24};
    REQUIRE(rig.ingress(b.end()));
    const auto r = rig.expect("/clockwork/record/start.reply");
    CHECK(r.parsed().argInt(0) == 0);
    CHECK_FALSE(r.parsed().argString(1).empty());
    CHECK_FALSE(std::filesystem::exists(path));
    CHECK_FALSE(rig.front.recording());
}

TEST_CASE("recording: the front going down finalises a recording in progress", "[front][recording]") {
    const std::string path = tempWav("front-record-teardown", "flac");
    {
        Rig rig;
        osc_test::Builder b;
        b.begin("/clockwork/record/start") << path.c_str() << "flac" << int32_t{16};
        REQUIRE(rig.ingress(b.end()));
        REQUIRE(rig.expect("/clockwork/record/start.reply").parsed().argInt(0) == 1);
        std::this_thread::sleep_for(std::chrono::milliseconds(120));
    }   // the front's destructor stops the recorder: the file must be whole
    const Decoded d = decode(path);
    CHECK(d.info.frames > 48000 * 0.05);
    CHECK(d.info.channels == 2);
    std::remove(path.c_str());
}

TEST_CASE("recording: the engine itself no longer answers the record verbs", "[front][recording]") {
    // Sent straight to the engine, past the front, the verb is unknown to
    // it: the engine has no files. (Its refusal shape is clockwork's.)
    EngineFixture fx;
    const std::string path = tempWav("engine-record");
    osc_test::Builder b;
    b.begin("/clockwork/record/start") << path.c_str() << "wav" << int32_t{24};
    fx.send(b.end());
    OscReply r;
    CHECK_FALSE(fx.waitForReply("/clockwork/record/start.reply", r, 500));
    CHECK_FALSE(std::filesystem::exists(path));
}

TEST_CASE("recording: what is playing is in the file",
          "[front][recording]") {
    Rig rig;
    REQUIRE(rig.fx.loadSynthDef("sonic-pi-beep"));
    const std::string path = tempWav("front-record-beep");
    osc_test::Builder b;
    b.begin("/clockwork/record/start") << path.c_str() << "wav" << int32_t{32};
    REQUIRE(rig.ingress(b.end()));
    REQUIRE(rig.expect("/clockwork/record/start.reply").parsed().argInt(0) == 1);
    {
        osc_test::Builder s;
        s.begin("/s_new") << "sonic-pi-beep" << int32_t{5000} << int32_t{0} << int32_t{0}
                          << "note" << 69.0f << "amp" << 1.0f << "sustain" << 2.0f << "out_bus" << 0.0f;
        rig.fx.send(s.end());
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    rig.sink.sent.clear();
    REQUIRE(rig.ingress(osc_test::message("/clockwork/record/stop")));
    REQUIRE(rig.expect("/clockwork/record/stop.reply").parsed().argInt(0) == 1);
    rig.fx.send(osc_test::message("/n_free", int32_t{5000}));

    const Decoded d = decode(path);
    REQUIRE(d.info.frames > 48000 * 0.2);
    float peak = 0.f;
    for (float v : d.samples) peak = std::max(peak, std::fabs(v));
    CHECK(peak > 0.05f);
    std::remove(path.c_str());
}

TEST_CASE("recording: a second, shorter recording after a longer one is whole",
          "[front][recording]") {
    Rig rig;
    auto record = [&](const std::string& path, int ms) {
        rig.sink.sent.clear();
        osc_test::Builder b;
        b.begin("/clockwork/record/start") << path.c_str() << "wav" << int32_t{16};
        REQUIRE(rig.ingress(b.end()));
        REQUIRE(rig.expect("/clockwork/record/start.reply").parsed().argInt(0) == 1);
        std::this_thread::sleep_for(std::chrono::milliseconds(ms));
        rig.sink.sent.clear();
        REQUIRE(rig.ingress(osc_test::message("/clockwork/record/stop")));
        REQUIRE(rig.expect("/clockwork/record/stop.reply").parsed().argInt(0) == 1);
    };
    const std::string first = tempWav("front-record-first"), second = tempWav("front-record-second");
    record(first, 600);
    record(second, 200);
    const Decoded a = decode(first), b = decode(second);
    CHECK(a.info.frames > 48000 * 0.4);
    CHECK(b.info.frames > 48000 * 0.1);   // not empty, not waiting for the last one's count
    CHECK(b.info.frames < 48000 * 0.5);
    std::remove(first.c_str());
    std::remove(second.c_str());
}

TEST_CASE("recording: every format and depth round-trips what plays, and the reply names the path",
          "[front][recording]") {
    Rig rig;
    REQUIRE(rig.fx.loadSynthDef("sonic-pi-beep"));
    struct Case { const char* header; int bits; const char* ext; };
    const Case cases[] = { { "wav", 16, "wav" }, { "wav", 24, "wav" }, { "wav", 32, "wav" }, { "flac", 16, "flac" }, { "flac", 24, "flac" } };
    for (const auto& c : cases) {
        DYNAMIC_SECTION(c.header << " " << c.bits) {
            const std::string path = tempWav("front-record-fmt", c.ext);
            rig.sink.sent.clear();
            osc_test::Builder b;
            b.begin("/clockwork/record/start") << path.c_str() << c.header << int32_t{c.bits};
            REQUIRE(rig.ingress(b.end()));
            const auto started = rig.expect("/clockwork/record/start.reply");
            REQUIRE(started.parsed().argInt(0) == 1);
            CHECK(started.parsed().argString(1) == path);
            osc_test::Builder sn;
            sn.begin("/s_new") << "sonic-pi-beep" << int32_t{5200} << int32_t{0} << int32_t{0}
                               << "note" << 60.0f << "amp" << 0.8f << "sustain" << 2.0f;
            rig.fx.send(sn.end());
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
            rig.sink.sent.clear();
            REQUIRE(rig.ingress(osc_test::message("/clockwork/record/stop")));
            const auto stopped = rig.expect("/clockwork/record/stop.reply");
            CHECK(stopped.parsed().argInt(0) == 1);
            CHECK(stopped.parsed().argString(1) == path);
            rig.fx.send(osc_test::message("/n_free", int32_t{5200}));
            std::this_thread::sleep_for(std::chrono::milliseconds(40));

            const Decoded d = decode(path);
            CHECK(d.info.channels == 2);
            CHECK(d.info.sample_rate == 48000);
            CHECK(d.info.frames > 48000 * 0.12);
            float peak = 0.f;
            for (float v : d.samples) peak = std::max(peak, std::fabs(v));
            CHECK(peak > 0.05f);
            CHECK(peak <= 1.0f);
            std::remove(path.c_str());
        }
    }
}

TEST_CASE("recording: three in a row, each the length it was asked for, none empty, none lost",
          "[front][recording]") {
    Rig rig;
    const int lengths[] = { 500, 150, 300 };
    for (int i = 0; i < 3; ++i) {
        const std::string path = tempWav(("front-record-seq-" + std::to_string(i)).c_str());
        rig.sink.sent.clear();
        osc_test::Builder b;
        b.begin("/clockwork/record/start") << path.c_str() << "wav" << int32_t{16};
        REQUIRE(rig.ingress(b.end()));
        REQUIRE(rig.expect("/clockwork/record/start.reply").parsed().argInt(0) == 1);
        std::this_thread::sleep_for(std::chrono::milliseconds(lengths[i]));
        rig.sink.sent.clear();
        REQUIRE(rig.ingress(osc_test::message("/clockwork/record/stop")));
        REQUIRE(rig.expect("/clockwork/record/stop.reply").parsed().argInt(0) == 1);
        CHECK(rig.front.recordingFramesLost() == 0);
        // Straight on to the next: no waiting for the guest to see the release.
        const Decoded d = decode(path);
        INFO("recording " << i << " asked " << lengths[i] << " ms, got " << d.info.frames << " frames");
        CHECK(d.info.frames > 48000 * (lengths[i] / 1000.0) * 0.5);
        CHECK(d.info.frames < 48000 * (lengths[i] / 1000.0) * 1.8 + 4800);
        std::remove(path.c_str());
    }
}
