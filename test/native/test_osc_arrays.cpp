/*
 * test_osc_arrays.cpp - control arrays in /s_new and /n_set.
 *
 * A control's values may be sent as an OSC array: '[' and ']' in the type
 * tags around the values. The message ends where its type tags or its data
 * end, whichever is first, so an array that is never closed, or whose values
 * run out, ends with the message and the engine goes on to the next one.
 *
 * Each message is written byte by byte: the client builders refuse to write
 * an unclosed array.
 */
#include "EngineFixture.h"

#include <cstring>
#include <string>
#include <vector>

namespace {

// An OSC message: address, type tags, then each argument as its tag says.
class RawOsc {
public:
    RawOsc(const char* address, const char* tags) { str(address); str(tags); }
    RawOsc& i(int32_t v) { for (int k = 3; k >= 0; --k) mBytes.push_back(uint8_t(uint32_t(v) >> (8 * k))); return *this; }
    RawOsc& f(float v)   { uint32_t u; std::memcpy(&u, &v, 4); return i(static_cast<int32_t>(u)); }
    RawOsc& str(const char* s) {
        const size_t n = std::strlen(s);
        mBytes.insert(mBytes.end(), s, s + n);
        do mBytes.push_back(0); while (mBytes.size() % 4 != 0);
        return *this;
    }
    osc_test::Packet packet() const { return osc_test::Packet{ mBytes }; }
private:
    std::vector<uint8_t> mBytes;
};

float ampOf(EngineFixture& fx, int32_t node) {
    osc_test::Builder b;
    b.begin("/s_get") << node << "amp";
    fx.clearReplies();
    fx.send(b.end());
    OscReply r;
    REQUIRE(fx.waitForReply("/n_set", r));
    return r.parsed().argFloat(2);
}

// The engine still answers after the message under test.
void stillRunning(EngineFixture& fx) {
    fx.send(osc_test::message("/sync", 91));
    OscReply synced;
    REQUIRE(fx.waitForReply("/synced", synced));
}

} // namespace

TEST_CASE("OSC arrays: a closed array in /s_new and /n_set sets the control",
          "[osc][arrays]") {
    EngineFixture fx;
    REQUIRE(fx.loadSynthDef("sonic-pi-beep"));

    fx.send(RawOsc("/s_new", ",siiis[f]").str("sonic-pi-beep").i(1001).i(0).i(0)
                .str("amp").f(0.25f).packet());
    CHECK(ampOf(fx, 1001) == 0.25f);

    fx.send(RawOsc("/n_set", ",is[f]").i(1001).str("amp").f(0.5f).packet());
    CHECK(ampOf(fx, 1001) == 0.5f);

    fx.send(osc_test::message("/n_free", 1001));
    stillRunning(fx);
}

TEST_CASE("OSC arrays: an array that is never closed ends with the message",
          "[osc][arrays]") {
    EngineFixture fx;
    REQUIRE(fx.loadSynthDef("sonic-pi-beep"));

    fx.send(RawOsc("/s_new", ",siiis[f").str("sonic-pi-beep").i(1002).i(0).i(0)
                .str("amp").f(0.25f).packet());
    stillRunning(fx);
    CHECK(ampOf(fx, 1002) == 0.25f);

    fx.send(RawOsc("/n_set", ",is[f").i(1002).str("amp").f(0.5f).packet());
    stillRunning(fx);
    CHECK(ampOf(fx, 1002) == 0.5f);

    fx.send(osc_test::message("/n_free", 1002));
}

TEST_CASE("OSC arrays: an array whose values run out ends with the message",
          "[osc][arrays]") {
    EngineFixture fx;
    REQUIRE(fx.loadSynthDef("sonic-pi-beep"));

    // Three values in the tags, one in the data.
    fx.send(RawOsc("/s_new", ",siiis[fff]").str("sonic-pi-beep").i(1003).i(0).i(0)
                .str("amp").f(0.25f).packet());
    stillRunning(fx);
    CHECK(ampOf(fx, 1003) == 0.25f);

    fx.send(RawOsc("/n_set", ",is[fff]").i(1003).str("amp").f(0.5f).packet());
    stillRunning(fx);
    CHECK(ampOf(fx, 1003) == 0.5f);

    fx.send(osc_test::message("/n_free", 1003));
}

TEST_CASE("OSC arrays: a value of a type controls do not take is passed over",
          "[osc][arrays]") {
    EngineFixture fx;
    REQUIRE(fx.loadSynthDef("sonic-pi-beep"));
    fx.send(RawOsc("/s_new", ",siiisf").str("sonic-pi-beep").i(1004).i(0).i(0)
                .str("amp").f(0.25f).packet());
    CHECK(ampOf(fx, 1004) == 0.25f);

    // 'T' (true) carries no data, and no control takes it. The name is long
    // enough that the controls loop is reached: it reads while 8 bytes remain.
    fx.send(RawOsc("/n_set", ",is[T]").i(1004).str("attack").packet());
    stillRunning(fx);
    CHECK(ampOf(fx, 1004) == 0.25f);

    fx.send(osc_test::message("/n_free", 1004));
}

