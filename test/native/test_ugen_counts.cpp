// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Sam Aaron
/*
 * test_ugen_counts.cpp - units that take a count from one of their own inputs:
 * the length of a reply's name, a number of channels, a number of envelope
 * stages. The unit then reads that many further inputs. A SynthDef can give
 * any count, so a count the unit's inputs cannot cover leaves the unit
 * silent: it reads none of them, and sends nothing.
 *
 * Each case plays a def that works, then defs that differ from it only in the
 * count.
 */
#include "SynthDefBytes.h"

#include <limits>
#include <set>
#include <string>
#include <utility>

using namespace synthdef_bytes;

namespace {

constexpr int8_t kControl = 1;
constexpr int8_t kAudio = 2;
constexpr int8_t kDemand = 3;

struct UnitSpec {
    const char* ugen;
    int8_t rate;
    std::vector<std::pair<int32_t, int32_t>> inputs; // (unit, output), or (-1, constant)
    int32_t outputs;
};

// Constants first through first + n - 1, as inputs.
std::vector<std::pair<int32_t, int32_t>> constants(int32_t first, int32_t n) {
    std::vector<std::pair<int32_t, int32_t>> in;
    for (int32_t i = 0; i < n; ++i)
        in.emplace_back(-1, first + i);
    return in;
}

std::vector<uint8_t> def(const char* name, const std::vector<float>& consts, const std::vector<UnitSpec>& units) {
    Def d;
    d.i32(0x53436766).i32(2).i16(1);
    d.str(name);
    d.i32(static_cast<int32_t>(consts.size()));
    for (const float c : consts) d.f32(c);
    d.i32(0);                                  // no controls
    d.i32(0);                                  // no control names
    d.i32(static_cast<int32_t>(units.size()));
    for (const auto& u : units) {
        d.str(u.ugen).i8(u.rate).i32(static_cast<int32_t>(u.inputs.size())).i32(u.outputs).i16(0);
        for (const auto& [unit, index] : u.inputs) d.i32(unit).i32(index);
        for (int32_t o = 0; o < u.outputs; ++o) d.i8(u.rate);
    }
    d.i16(0);                                  // no variants
    return d.bytes();
}

// The characters of `s` as constants.
std::vector<float> chars(const char* s) {
    std::vector<float> c;
    for (; *s; ++s) c.push_back(static_cast<float>(*s));
    return c;
}

std::vector<float> join(std::vector<float> a, const std::vector<float>& b) {
    a.insert(a.end(), b.begin(), b.end());
    return a;
}

// Node replies reach clients that asked for notifications.
void notify(EngineFixture& fx) {
    fx.send(osc_test::message("/notify", 1));
    OscReply done;
    REQUIRE(fx.waitForReply("/done", done));
}

// Plays `name` as node 2000 for `blocks` blocks, then frees it. Returns the
// addresses of the replies that arrived meanwhile.
std::multiset<std::string> play(EngineFixture& fx, const char* name, uint32_t blocks = 64) {
    fx.clearReplies();
    osc_test::Builder b;
    b.begin("/s_new") << name << 2000 << 0 << 0;
    fx.send(b.end());
    fx.pumpBlock(blocks);
    fx.send(osc_test::message("/n_free", 2000));
    fx.pumpBlock(2);
    std::multiset<std::string> seen;
    for (const auto& r : fx.allReplies()) seen.insert(r.address);
    return seen;
}

// Replies the server sends for its own reasons, whatever the def.
std::multiset<std::string> unitReplies(std::multiset<std::string> seen) {
    for (const char* own : {"/n_go", "/n_end", "/n_on", "/n_off", "/synced", "/done", "/fail"})
        seen.erase(own);
    return seen;
}

// SendReply.kr: trigger, reply id, name length, name, values.
std::vector<uint8_t> sendReply(const char* name, float nameLength, const char* replyName, int32_t values) {
    auto c = join({1.0f, 0.0f, nameLength}, chars(replyName));
    for (int32_t v = 0; v < values; ++v) c.push_back(0.5f);
    return def(name, c, {{"SendReply", kControl, constants(0, static_cast<int32_t>(c.size())), 0}});
}

// SendPeakRMS.kr: reply rate, lag, reply id, channel count, channels, name
// length, name.
std::vector<uint8_t> sendPeakRMS(const char* name, float channelCount, int32_t channels, float nameLength,
                                 const char* replyName) {
    std::vector<float> c = {20.0f, 3.0f, 0.0f, channelCount};
    for (int32_t ch = 0; ch < channels; ++ch) c.push_back(0.5f);
    c.push_back(nameLength);
    c = join(c, chars(replyName));
    return def(name, c, {{"SendPeakRMS", kControl, constants(0, static_cast<int32_t>(c.size())), 0}});
}

// SendPeakRMS.ar with one channel, replying `replyRate` times a second.
std::vector<uint8_t> sendPeakRMSAudio(const char* name, float replyRate) {
    const auto c = join({replyRate, 3.0f, 0.0f, 1.0f, 0.5f, 3.0f}, chars("/pk"));
    return def(name, c, {{"SendPeakRMS", kAudio, constants(0, static_cast<int32_t>(c.size())), 0}});
}

// Poll.kr: trigger, value, trigger id, label length, label.
std::vector<uint8_t> poll(const char* name, float labelLength, const char* label) {
    const auto c = join({1.0f, 0.5f, 7.0f, labelLength}, chars(label));
    return def(name, c, {{"Poll", kControl, constants(0, static_cast<int32_t>(c.size())), 1}});
}

// Demand.kr pulling Dpoll: value, trigger id, run, label length, label.
std::vector<uint8_t> dpoll(const char* name, float labelLength, const char* label) {
    auto c = join({0.5f, 7.0f, 1.0f, labelLength}, chars(label));
    const auto n = static_cast<int32_t>(c.size());
    c.push_back(1.0f); // Demand's trigger
    c.push_back(0.0f); // and reset
    return def(name, c, {{"Dpoll", kDemand, constants(0, n), 1},
                         {"Demand", kControl, {{-1, n}, {-1, n + 1}, {0, 0}}, 1}});
}

// IEnvGen.kr into Out.kr on control bus 0: index, offset, initial level,
// stage count, total duration, then level, duration, shape and curve for each
// stage given.
std::vector<uint8_t> ienvGen(const char* name, float stageCount, int32_t stages) {
    std::vector<float> c = {0.0f, 0.0f, 0.25f, stageCount, 1.0f};
    for (int32_t s = 0; s < stages; ++s) {
        c.push_back(0.25f); c.push_back(1.0f); c.push_back(1.0f); c.push_back(0.0f);
    }
    const auto n = static_cast<int32_t>(c.size());
    c.push_back(0.0f); // Out's bus
    return def(name, c, {{"IEnvGen", kControl, constants(0, n), 1},
                         {"Out", kControl, {{-1, n}, {0, 0}}, 0}});
}

} // namespace

TEST_CASE("UGen counts: SendReply with a name its inputs do not hold sends nothing", "[ugen_counts]") {
    EngineFixture fx;
    notify(fx);
    REQUIRE(loads(fx, sendReply("sr_ok", 3, "/sr", 2)));
    CHECK(unitReplies(play(fx, "sr_ok")) == std::multiset<std::string>{"/sr"});

    REQUIRE(loads(fx, sendReply("sr_name_past_values", 4, "/sr", 0)));
    CHECK(unitReplies(play(fx, "sr_name_past_values")).empty());
    REQUIRE(loads(fx, sendReply("sr_name_far_past", 400, "/sr", 2)));
    CHECK(unitReplies(play(fx, "sr_name_far_past")).empty());
    REQUIRE(loads(fx, sendReply("sr_name_negative", -1, "/sr", 2)));
    CHECK(unitReplies(play(fx, "sr_name_negative")).empty());
    poolStillSound(fx);
}

TEST_CASE("UGen counts: SendPeakRMS with channels or a name its inputs do not hold sends nothing",
          "[ugen_counts]") {
    EngineFixture fx;
    notify(fx);
    REQUIRE(loads(fx, sendPeakRMS("pk_ok", 2, 2, 3, "/pk")));
    CHECK(unitReplies(play(fx, "pk_ok")).count("/pk") > 0);

    REQUIRE(loads(fx, sendPeakRMS("pk_channels_past", 3, 2, 3, "/pk")));
    CHECK(unitReplies(play(fx, "pk_channels_past")).empty());
    REQUIRE(loads(fx, sendPeakRMS("pk_channels_far_past", 1.4316558e9f, 2, 3, "/pk")));
    CHECK(unitReplies(play(fx, "pk_channels_far_past")).empty());
    REQUIRE(loads(fx, sendPeakRMS("pk_channels_negative", -1, 2, 3, "/pk")));
    CHECK(unitReplies(play(fx, "pk_channels_negative")).empty());
    REQUIRE(loads(fx, sendPeakRMS("pk_name_past", 2, 2, 9, "/pk")));
    CHECK(unitReplies(play(fx, "pk_name_past")).empty());
    REQUIRE(loads(fx, sendPeakRMS("pk_name_negative", 2, 2, -1, "/pk")));
    CHECK(unitReplies(play(fx, "pk_name_negative")).empty());
    poolStillSound(fx);
}

TEST_CASE("UGen counts: SendPeakRMS replying faster than the sample rate replies once a sample",
          "[ugen_counts]") {
    EngineFixture fx;
    notify(fx);
    for (const auto& [name, rate] : {std::pair{"pk_rate_past_sample_rate", 1e9f},
                                     std::pair{"pk_rate_negative", -20.0f},
                                     std::pair{"pk_rate_nan", std::numeric_limits<float>::quiet_NaN()}}) {
        REQUIRE(loads(fx, sendPeakRMSAudio(name, rate)));
        play(fx, name, 2);
    }
    poolStillSound(fx);
}

TEST_CASE("UGen counts: Poll and Dpoll with a label their inputs do not hold send nothing", "[ugen_counts]") {
    EngineFixture fx;
    notify(fx);
    REQUIRE(loads(fx, poll("poll_ok", 1, "x")));
    CHECK(unitReplies(play(fx, "poll_ok")).count("/tr") > 0);
    REQUIRE(loads(fx, dpoll("dpoll_ok", 1, "x")));
    CHECK(unitReplies(play(fx, "dpoll_ok")).count("/tr") > 0);

    REQUIRE(loads(fx, poll("poll_label_past", 60, "x")));
    CHECK(unitReplies(play(fx, "poll_label_past")).empty());
    REQUIRE(loads(fx, poll("poll_label_negative", -1, "x")));
    CHECK(unitReplies(play(fx, "poll_label_negative")).empty());
    REQUIRE(loads(fx, dpoll("dpoll_label_past", 60, "x")));
    CHECK(unitReplies(play(fx, "dpoll_label_past")).empty());
    REQUIRE(loads(fx, dpoll("dpoll_label_negative", -1, "x")));
    CHECK(unitReplies(play(fx, "dpoll_label_negative")).empty());
    poolStillSound(fx);
}

TEST_CASE("UGen counts: IEnvGen with stages its inputs do not hold outputs nothing", "[ugen_counts]") {
    EngineFixture fx;
    notify(fx);
    REQUIRE(loads(fx, ienvGen("ienv_ok", 1, 1)));
    play(fx, "ienv_ok", 4);
    CHECK(controlBus(fx, 0) == 0.25f);

    for (const auto& [name, count] : {std::pair{"ienv_stages_past", 40.0f},
                                      std::pair{"ienv_stages_wrap", 1.0737418e9f},
                                      std::pair{"ienv_stages_negative", -2.0f}}) {
        osc_test::Builder set;
        set.begin("/c_set") << 0 << 0.75f;
        fx.send(set.end());
        fx.pumpBlock(1);
        REQUIRE(loads(fx, ienvGen(name, count, 1)));
        play(fx, name, 4);
        CHECK(controlBus(fx, 0) == 0.0f);
    }
    poolStillSound(fx);
}
