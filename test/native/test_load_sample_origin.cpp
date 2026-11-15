/*
 * test_load_sample_origin.cpp — a buffer's answer carries its asker's token.
 *
 * A client that asks for a buffer matches the answer to its ask by the token
 * it sent the ask with. These cases send with a token and read it back off
 * the answer, as the fixture records it (OscReply::token).
 *
 * TWO WAYS A SAMPLE ARRIVES, ONE PLACE IT IS DECODED. The client decodes the
 * file (clockwork_audio_file.h), lays the frames out in the ASSETS with the
 * guard frames the interpolators read past, and either commits the slot as an
 * asset keyed by buffer number (/clockwork/asset/commit — SampleLane.h) or
 * commits the offset as an asset. The engine reads no files: /b_allocRead
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
#include <string>
#include <thread>
#include <vector>

#ifndef CLOCKWORK_SAMPLES_DIR
#define CLOCKWORK_SAMPLES_DIR ""
#endif

namespace {

constexpr uint32_t kAsker = 0xA5C1;   // any non-zero token: 0 is no token

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

TEST_CASE("asset commit: a refusal for a range that leaves the assets area carries the asker's token",
          "[load_sample][origin][lane]") {
    // The offset is trusted by nobody. A range past the end of the lane would
    // be read as sample data from whatever lies beyond it, so it is refused —
    // and the refusal still has to reach the client that sent it.
    EngineFixture fx;
    const uint32_t assetsBytes = fx.engine().guestAssetsBytes();
    REQUIRE(assetsBytes > 0);

    osc_test::Builder b;
    auto& m = b.begin("/clockwork/asset/commit");
    // One frame short of the end would fit; two frames run past it.
    m << int32_t{0} << int32_t{1} << static_cast<int32_t>(assetsBytes - sizeof(float))
      << int32_t{8} << int32_t{1} << int32_t{2} << 44100.0f;
    const auto msg = b.end();
    fx.engine().ingest(msg.ptr(), msg.size(), kAsker);

    OscReply refused;
    REQUIRE(fx.waitForReply("/clockwork/asset/refused", refused));
    // The answer carries the token its message was sent with.
    CHECK(refused.token == kAsker);
}

TEST_CASE("asset commit: /clockwork/asset/committed carries the asker's token",
          "[load_sample][origin]") {
    if (!haveSample("bd_haus.flac")) SKIP("sample bd_haus.flac not found");
    EngineFixture fx;


    // Staged by this client; the one message is sent as kAsker.
    const sample_lane::Staged st = sample_lane::stage(fx, 0, samplePath("bd_haus.flac"));
    REQUIRE(st.ok);
    fx.engine().ingest(st.commit.ptr(), st.commit.size(), kAsker);

    OscReply done;
    REQUIRE(fx.waitForReply("/clockwork/asset/committed", done));
    // The answer carries the token its message was sent with.
    CHECK(done.token == kAsker);
}

TEST_CASE("/b_allocRead: the refusal carries the asker's token",
          "[load_sample][origin]") {
    EngineFixture fx;


    // The engine reads no files: the command takes its failure branch for any
    // path — which is the half a client most needs to hear about, and the
    // half most easily left on the wrong route.
    const auto msg = allocRead(0, samplePath("bd_haus.flac"));
    fx.engine().ingest(msg.ptr(), msg.size(), kAsker);

    OscReply fail;
    REQUIRE(fx.waitForReply("/fail", fail));

    // The answer carries the token its message was sent with.
    CHECK(fail.token == kAsker);
}
