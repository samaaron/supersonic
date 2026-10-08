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
#include "supersonic_commands.h"
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
    bool subscribeNotifyPort(int) override { return false; }
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

// The recordings here are measured in the engine's frames, never the wall clock's. The tap counts every frame
// clockwork writes to it (write_position), and with manualAudioPump nothing renders but this thread, a block at a
// time, so a recording holds exactly the frames rendered between its start and its stop however slow the machine is.
// They were sleeps once, with bounds on the time slept, and a loaded runner rendered a fraction of that time: a file
// of 197 ms where 200 was the least (a macOS runner, 0.89.0's release). One recording, the last, is made against the
// headless driver on its own thread, as a device would render, and is bounded by the same counter.
ClockworkEngine::Config pumpedConfig() {
    auto cfg = EngineFixture::defaultConfig();
    cfg.manualAudioPump = true;
    return cfg;
}

struct Rig {
    EngineFixture    fx;
    RecordingSink    sink;
    supersonic::Commands front;
    shm_audio_buffer* slot0 = nullptr;
    explicit Rig(const ClockworkEngine::Config& cfg = pumpedConfig()) : fx(cfg), front(fx.engine(), &sink) {
        fx.setRoutedObserver([this](uint32_t origin, uint32_t, const uint8_t* d, uint32_t n) {
            if (!front.egress(origin, d, n)) sink.send(origin, d, n, false);
        });
        ClockworkRegion taps {};
        REQUIRE(clockwork_client_region(fx.engine().egressClient(), CLOCKWORK_REGION_AUDIO_TAPS, &taps) == CLOCKWORK_OK);
        slot0 = static_cast<shm_audio_buffer*>(taps.base);
        REQUIRE(slot0->enabled.load() == 1);   // formatted at boot: the tap is live before the first block
    }
    ~Rig() { fx.setRoutedObserver(nullptr); }   // under the fixture's lock: no call runs after this
    bool ingress(const osc_test::Packet& p) { return front.ingress(p.ptr(), p.size(), kAsker); }
    OscReply expect(const char* addr) {
        OscReply r;
        REQUIRE(sink.waitFor(addr, kAsker, &r));
        return r;
    }
    // Every frame clockwork has written to the tap since boot.
    uint64_t written() const { return slot0->write_position.load(std::memory_order_acquire); }
    // Render at least `seconds` of audio on this thread, a block at a time; the frames rendered.
    uint64_t render(double seconds) {
        const uint64_t from = written();
        const uint64_t want = static_cast<uint64_t>(seconds * slot0->sample_rate);
        while (written() - from < want) {
            const uint64_t before = written();
            fx.pumpBlock();
            REQUIRE(written() > before);   // a block that wrote nothing to the tap would never end this
        }
        return written() - from;
    }
    // Start recording `path`; true when the front said it had.
    bool start(const std::string& path, const char* header = "wav", int bits = 16) {
        sink.sent.clear();
        osc_test::Builder b;
        b.begin("/clockwork/record/start") << path.c_str() << header << int32_t{bits};
        if (!ingress(b.end())) return false;
        return expect("/clockwork/record/start.reply").parsed().argInt(0) == 1;
    }
    bool stop() {
        sink.sent.clear();
        if (!ingress(osc_test::message("/clockwork/record/stop"))) return false;
        return expect("/clockwork/record/stop.reply").parsed().argInt(0) == 1;
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

float peakOf(const Decoded& d) {
    float peak = 0.f;
    for (float v : d.samples) peak = std::max(peak, std::fabs(v));
    return peak;
}

} // namespace

TEST_CASE("recording: /clockwork/record/start and /stop write clockwork's OUT tap to a file, on the client's thread",
          "[front][recording]") {
    Rig rig;
    const std::string path = tempWav("front-record");
    // The tap has been flowing since boot; what it carried before the start is not the recording's.
    rig.render(0.05);
    osc_test::Builder b;
    b.begin("/clockwork/record/start") << path.c_str() << "wav" << int32_t{32};
    REQUIRE(rig.ingress(b.end()));
    const auto started = rig.expect("/clockwork/record/start.reply");
    const uint64_t from = rig.written();
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
    const uint64_t rendered = rig.render(0.3);

    rig.sink.sent.clear();
    REQUIRE(rig.ingress(osc_test::message("/clockwork/record/stop")));
    const auto stopped = rig.expect("/clockwork/record/stop.reply");
    const uint64_t to = rig.written();
    CHECK(stopped.parsed().argInt(0) == 1);
    CHECK(stopped.parsed().argString(1) == path);
    CHECK_FALSE(rig.front.recording());

    // The file is complete, at the tap's geometry: every frame clockwork wrote between the start and the stop, none
    // from before, none lost, none twice (silence: nothing is playing).
    const Decoded d = decode(path);
    INFO("rendered " << rendered << " frames from " << from << " to " << to << ", the file has " << d.info.frames);
    CHECK(d.info.channels == 2);
    CHECK(d.info.sample_rate == 48000);
    CHECK(d.info.frames == to - from);
    CHECK(rig.front.recordingFramesLost() == 0);
    rig.render(0.01);
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
    uint64_t recorded = 0;
    {
        Rig rig;
        REQUIRE(rig.start(path, "flac", 16));
        const uint64_t from = rig.written();
        rig.render(0.12);
        recorded = rig.written() - from;
    }   // the front's destructor stops the recorder: the file must be whole
    const Decoded d = decode(path);
    CHECK(d.info.frames == recorded);
    CHECK(d.info.channels == 2);
    std::remove(path.c_str());
}

TEST_CASE("recording: the engine itself no longer answers the record verbs", "[front][recording]") {
    // Sent straight to the engine, past the front, the verb is unknown to
    // it: the engine has no files. (Its refusal shape is clockwork's.) The
    // wait renders blocks of its own (manualAudioPump), so the engine has
    // handled the message long before it ends: no reply is an answer.
    EngineFixture fx(pumpedConfig());
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
    REQUIRE(rig.start(path, "wav", 32));
    const uint64_t from = rig.written();
    {
        osc_test::Builder s;
        s.begin("/s_new") << "sonic-pi-beep" << int32_t{5000} << int32_t{0} << int32_t{0}
                          << "note" << 69.0f << "amp" << 1.0f << "sustain" << 2.0f << "out_bus" << 0.0f;
        rig.fx.send(s.end());
    }
    rig.render(0.4);
    REQUIRE(rig.stop());
    const uint64_t to = rig.written();
    rig.fx.send(osc_test::message("/n_free", int32_t{5000}));

    const Decoded d = decode(path);
    REQUIRE(d.info.frames == to - from);
    CHECK(peakOf(d) > 0.05f);
    std::remove(path.c_str());
}

TEST_CASE("recording: a second, shorter recording after a longer one is whole",
          "[front][recording]") {
    Rig rig;
    auto record = [&](const std::string& path, double seconds) {
        REQUIRE(rig.start(path));
        const uint64_t from = rig.written();
        rig.render(seconds);
        REQUIRE(rig.stop());
        return rig.written() - from;
    };
    const std::string first = tempWav("front-record-first"), second = tempWav("front-record-second");
    const uint64_t firstFrames = record(first, 0.6);
    const uint64_t secondFrames = record(second, 0.2);
    const Decoded a = decode(first), b = decode(second);
    CHECK(a.info.frames == firstFrames);
    CHECK(b.info.frames == secondFrames);   // not empty, not waiting for the last one's count
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
            const uint64_t from = rig.written();
            osc_test::Builder sn;
            sn.begin("/s_new") << "sonic-pi-beep" << int32_t{5200} << int32_t{0} << int32_t{0}
                               << "note" << 60.0f << "amp" << 0.8f << "sustain" << 2.0f;
            rig.fx.send(sn.end());
            rig.render(0.25);
            rig.sink.sent.clear();
            REQUIRE(rig.ingress(osc_test::message("/clockwork/record/stop")));
            const auto stopped = rig.expect("/clockwork/record/stop.reply");
            const uint64_t to = rig.written();
            CHECK(stopped.parsed().argInt(0) == 1);
            CHECK(stopped.parsed().argString(1) == path);
            rig.fx.send(osc_test::message("/n_free", int32_t{5200}));

            const Decoded d = decode(path);
            CHECK(d.info.channels == 2);
            CHECK(d.info.sample_rate == 48000);
            CHECK(d.info.frames == to - from);
            const float peak = peakOf(d);
            CHECK(peak > 0.05f);
            CHECK(peak <= 1.0f);
            std::remove(path.c_str());
        }
    }
}

TEST_CASE("recording: three in a row, each the length it was asked for, none empty, none lost",
          "[front][recording]") {
    Rig rig;
    const double lengths[] = { 0.5, 0.15, 0.3 };
    for (int i = 0; i < 3; ++i) {
        const std::string path = tempWav(("front-record-seq-" + std::to_string(i)).c_str());
        REQUIRE(rig.start(path));
        const uint64_t from = rig.written();
        rig.render(lengths[i]);
        REQUIRE(rig.stop());
        const uint64_t recorded = rig.written() - from;
        CHECK(rig.front.recordingFramesLost() == 0);
        // Straight on to the next: no waiting for the guest to see the release.
        const Decoded d = decode(path);
        INFO("recording " << i << " asked " << lengths[i] << " s, rendered " << recorded << " frames, got " << d.info.frames);
        CHECK(recorded >= static_cast<uint64_t>(lengths[i] * 48000));
        CHECK(d.info.frames == recorded);
        std::remove(path.c_str());
    }
}

TEST_CASE("recording: against the live audio thread, the file holds what clockwork wrote between the start and the stop",
          "[front][recording]") {
    // The headless driver renders on its own thread, as a device does, while the recorder reads the tap. Bounded in
    // the engine's frames all the same: at least what was written between the start's reply and the stop's request,
    // at most what was written between the start's request and the stop's reply. A frame the ring lapped is counted
    // (framesLost), never hidden, so the frames written and lost together are bounded the same way.
    Rig rig(EngineFixture::defaultConfig());
    const std::string path = tempWav("front-record-live");
    const uint64_t beforeStart = rig.written();
    REQUIRE(rig.start(path));
    const uint64_t afterStart = rig.written();
    REQUIRE(rig.fx.waitForBlocks(64, 60000));   // the driver's blocks, not the clock's milliseconds
    const uint64_t beforeStop = rig.written();
    REQUIRE(rig.stop());
    const uint64_t afterStop = rig.written();

    const Decoded d = decode(path);
    const uint64_t accounted = d.info.frames + rig.front.recordingFramesLost();
    INFO("tap at " << beforeStart << " / " << afterStart << " around the start, " << beforeStop << " / " << afterStop
         << " around the stop; the file has " << d.info.frames << ", " << rig.front.recordingFramesLost() << " lost");
    CHECK(d.info.frames > 0);
    CHECK(accounted >= beforeStop - afterStart);
    CHECK(accounted <= afterStop - beforeStart);
    std::remove(path.c_str());
}
