/*
 * test_load_sample_origin.cpp — a buffer reply answers the client that asked.
 *
 * A client that asks for a buffer has to be the one told the answer. An origin
 * of 0 would not broadcast it: 0 addresses the clients that sent
 * /clockwork/notify, and asking for a buffer does not make a client one of
 * them.
 *
 * onReply cannot see which of those happened — in-process a reply and a
 * notification both arrive through it — so these read onReplyRouted, which
 * carries the origin and the route that decided where the message went.
 *
 * TWO WAYS A SAMPLE ARRIVES, ONE PLACE IT IS DECODED. The client decodes the
 * file (clockwork_audio_file.h), lays the frames out in the INBOX with the
 * guard frames the interpolators read past, and either commits the slot as an
 * asset keyed by buffer number (/clockwork/asset/commit — SampleLane.h) or
 * names the offset with /b_allocPtr. The engine reads no files: /b_allocRead
 * is refused, and the refusal, like every other answer, goes to whoever asked.
 * These cases exercise all three, natively, with a real file.
 */
#include "EngineFixture.h"
#include "SampleLane.h"
#include "clockwork_audio_file.h"
#include "buffer_commands.h"

#include <cstring>

#include <chrono>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifndef CLOCKWORK_SAMPLES_DIR
#define CLOCKWORK_SAMPLES_DIR ""
#endif

namespace {

constexpr uint32_t kAsker = 0xA5C1;   // any non-zero token: 0 is the notify audience

struct Routed {
    uint32_t    origin;
    uint32_t    route;
    std::string address;
};

std::string samplePath(const char* name) {
    return std::string(CLOCKWORK_SAMPLES_DIR) + "/" + name;
}

bool haveSample(const char* name) {
    return std::filesystem::exists(std::filesystem::path(samplePath(name)));
}

osc_test::Packet allocRead(int32_t buf, const std::string& path) {
    osc_test::Builder b;
    auto& s = b.begin("/b_allocRead");
    s << buf << path.c_str() << int32_t{0} << int32_t{0};
    return b.end();
}

} // namespace

TEST_CASE("/b_allocPtr: a sample handed over through the inbox is answered to the asker",
          "[load_sample][origin][lane]") {
    // SKIP, not a silent return: a case that quietly passes when the sample
    // is absent asserts nothing and says so to nobody.
    if (!haveSample("bd_haus.flac")) SKIP("sample bd_haus.flac not found");

    EngineFixture fx;

    // The client's half: decode, then lay out [guard][audio][guard] where the
    // engine will read it, with the guard layout from its one definition.
    ClockworkAudioInfo info{};
    info.struct_bytes = sizeof(info);
    float* pcm = nullptr;
    REQUIRE(clockwork_audio_decode_file(samplePath("bd_haus.flac").c_str(), &info, &pcm)
            == CLOCKWORK_OK);
    REQUIRE(pcm != nullptr);
    REQUIRE(info.frames > 0);

    uint8_t* inbox = const_cast<uint8_t*>(fx.engine().guestInbox());
    const uint32_t inboxBytes = fx.engine().guestInboxBytes();
    REQUIRE(inbox != nullptr);

    const uint32_t gb = supersonic_buffer_guard_before();
    const uint32_t ga = supersonic_buffer_guard_after();
    const size_t   ch = info.channels;
    const size_t   floats = (gb + info.frames + ga) * ch;
    const size_t   bytes  = floats * sizeof(float);
    if (bytes > inboxBytes) { clockwork_audio_free(pcm); SKIP("sample larger than this engine's inbox"); }

    const uint32_t offset = 0;
    float* dst = reinterpret_cast<float*>(inbox + offset);
    std::memset(dst, 0, bytes);
    std::memcpy(dst + gb * ch, pcm, info.frames * ch * sizeof(float));
    clockwork_audio_free(pcm);

    std::mutex mu;
    std::vector<Routed> seen;
    fx.engine().onReplyRouted = [&](uint32_t origin, uint32_t route,
                                    const uint8_t* d, uint32_t n) {
        std::lock_guard<std::mutex> lock(mu);
        seen.push_back({origin, route, osc_test::parseAddress(d, n)});
    };

    osc_test::Builder b;
    auto& m = b.begin("/b_allocPtr");
    m << int32_t{0} << static_cast<int32_t>(offset)
      << static_cast<int32_t>(info.frames) << static_cast<int32_t>(info.channels)
      << static_cast<float>(info.sample_rate) << "lane-test";
    const auto msg = b.end();

    // ingest(), not send(): the token is the whole point, and sendOSC has none.
    fx.engine().ingest(msg.ptr(), msg.size(), kAsker);

    OscReply done;
    REQUIRE(fx.waitForReply("/done", done));

    std::lock_guard<std::mutex> lock(mu);
    const Routed* reply = nullptr;
    for (const auto& r : seen)
        if (r.address == "/done") { reply = &r; break; }
    REQUIRE(reply != nullptr);
    // Addressed to the asker, by the reply route. With the origin dropped this
    // is {0, EGRESS_BROADCAST_NOTIFY} and the /done goes to the notify
    // audience — which, with nobody subscribed, is nobody at all.
    CHECK(reply->origin == kAsker);
    CHECK(reply->route  == static_cast<uint32_t>(EGRESS_REPLY));
}

TEST_CASE("/b_allocPtr: a range that leaves the inbox is refused, to the asker",
          "[load_sample][origin][lane]") {
    // The offset is trusted by nobody. A range past the end of the lane would
    // be read as sample data from whatever lies beyond it, so it is refused —
    // and the refusal still has to reach the client that sent it.
    EngineFixture fx;
    const uint32_t inboxBytes = fx.engine().guestInboxBytes();
    REQUIRE(inboxBytes > 0);

    std::mutex mu;
    std::vector<Routed> seen;
    fx.engine().onReplyRouted = [&](uint32_t origin, uint32_t route,
                                    const uint8_t* d, uint32_t n) {
        std::lock_guard<std::mutex> lock(mu);
        seen.push_back({origin, route, osc_test::parseAddress(d, n)});
    };

    osc_test::Builder b;
    auto& m = b.begin("/b_allocPtr");
    // One frame short of the end would fit; the guard frames push it over.
    m << int32_t{0} << static_cast<int32_t>(inboxBytes - sizeof(float))
      << int32_t{1} << int32_t{1} << 44100.0f << "past-the-end";
    const auto msg = b.end();
    fx.engine().ingest(msg.ptr(), msg.size(), kAsker);

    OscReply fail;
    REQUIRE(fx.waitForReply("/fail", fail));
    std::lock_guard<std::mutex> lock(mu);
    const Routed* reply = nullptr;
    for (const auto& r : seen)
        if (r.address == "/fail") { reply = &r; break; }
    REQUIRE(reply != nullptr);
    CHECK(reply->origin == kAsker);
    CHECK(reply->route  == static_cast<uint32_t>(EGRESS_REPLY));
}

TEST_CASE("asset commit: /clockwork/asset/committed is addressed to the client that asked",
          "[load_sample][origin]") {
    if (!haveSample("bd_haus.flac")) SKIP("sample bd_haus.flac not found");
    EngineFixture fx;

    std::mutex mu;
    std::vector<Routed> seen;
    fx.engine().onReplyRouted = [&](uint32_t origin, uint32_t route,
                                    const uint8_t* d, uint32_t n) {
        std::lock_guard<std::mutex> lock(mu);
        seen.push_back({origin, route, osc_test::parseAddress(d, n)});
    };

    // Staged by this client; the one message is sent as kAsker.
    const sample_lane::Staged st = sample_lane::stage(fx, 0, samplePath("bd_haus.flac"));
    REQUIRE(st.ok);
    fx.engine().ingest(st.commit.ptr(), st.commit.size(), kAsker);

    OscReply done;
    REQUIRE(fx.waitForReply("/clockwork/asset/committed", done));
    std::lock_guard<std::mutex> lock(mu);
    const Routed* reply = nullptr;
    for (const auto& r : seen)
        if (r.address == "/clockwork/asset/committed") { reply = &r; break; }
    REQUIRE(reply != nullptr);
    CHECK(reply->origin == kAsker);
    CHECK(reply->route  == static_cast<uint32_t>(EGRESS_REPLY));
}

TEST_CASE("/b_allocRead: the refusal is addressed to the client that asked",
          "[load_sample][origin]") {
    EngineFixture fx;

    std::mutex mu;
    std::vector<Routed> seen;
    fx.engine().onReplyRouted = [&](uint32_t origin, uint32_t route,
                                    const uint8_t* d, uint32_t n) {
        std::lock_guard<std::mutex> lock(mu);
        seen.push_back({origin, route, osc_test::parseAddress(d, n)});
    };

    // The engine reads no files: the command takes its failure branch for any
    // path — which is the half a client most needs to hear about, and the
    // half most easily left on the wrong route.
    const auto msg = allocRead(0, samplePath("bd_haus.flac"));
    fx.engine().ingest(msg.ptr(), msg.size(), kAsker);

    OscReply fail;
    REQUIRE(fx.waitForReply("/fail", fail));

    std::lock_guard<std::mutex> lock(mu);
    const Routed* reply = nullptr;
    for (const auto& r : seen)
        if (r.address == "/fail") { reply = &r; break; }

    REQUIRE(reply != nullptr);
    CHECK(reply->origin == kAsker);
    CHECK(reply->route  == static_cast<uint32_t>(EGRESS_REPLY));
}
