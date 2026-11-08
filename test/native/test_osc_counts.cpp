/*
 * test_osc_counts.cpp - counts a client sends in /n_mapn, /n_mapan, /n_fill,
 * /s_getn, /c_getn, /b_getn and the fill and setn commands.
 *
 * Each handler loops over a count from the message. The loops over controls
 * stop at the most controls any synth under the node has, so a count of
 * INT32_MAX costs no more than the controls that exist; with the count
 * unbounded, the root group's ten synths take minutes on the audio thread and
 * the fixture's 30 s guard ends the case. The getn handlers reserve their
 * reply's type tags from the counts, so a negative count is refused before
 * any tag is reserved.
 *
 * /n_set, /n_setn, /s_getn, /s_new and /s_newargs take a run of controls from
 * an index the client sends, so they reach index + i. Past INT32_MAX that sum
 * is undefined in C++, and only a UBSan build that halts on its first report
 * sees it. CI's sanitizer job runs that way.
 */
#include "EngineFixture.h"

#include <cstdint>

namespace {

constexpr int32_t kHuge = INT32_MAX;

osc_test::Packet sNew(int32_t id) {
    osc_test::Builder b;
    b.begin("/s_new") << "sonic-pi-beep" << id << 0 << 0;
    return b.end();
}

float controlOf(EngineFixture& fx, int32_t node, const char* name) {
    osc_test::Builder b;
    b.begin("/s_get") << node << name;
    fx.clearReplies();
    fx.send(b.end());
    OscReply r;
    REQUIRE(fx.waitForReply("/n_set", r));
    return r.parsed().argFloat(2);
}

void stillRunning(EngineFixture& fx) {
    fx.send(osc_test::message("/sync", 93));
    OscReply synced;
    REQUIRE(fx.waitForReply("/synced", synced));
}

void refused(EngineFixture& fx, const osc_test::Packet& p, const char* cmd) {
    fx.clearReplies();
    fx.send(p);
    OscReply r;
    REQUIRE(fx.waitForReply("/fail", r));
    CHECK(r.parsed().argString(0) == cmd);
}

} // namespace

TEST_CASE("OSC counts: /n_fill, /n_mapn and /n_mapan loop no further than the controls",
          "[osc][counts]") {
    EngineFixture fx;
    REQUIRE(fx.loadSynthDef("sonic-pi-beep"));
    for (int32_t id = 1000; id < 1010; ++id)
        fx.send(sNew(id));

    {
        osc_test::Builder b;
        b.begin("/n_fill") << 0 << "amp" << kHuge << 0.25f;
        fx.send(b.end());
    }
    stillRunning(fx);
    CHECK(controlOf(fx, 1009, "amp") == 0.25f);

    {
        osc_test::Builder b;
        b.begin("/n_fill") << 0 << 0 << kHuge << 0.5f;
        fx.send(b.end());
    }
    stillRunning(fx);
    CHECK(controlOf(fx, 1009, "amp") == 0.5f);

    for (const char* cmd : {"/n_mapn", "/n_mapan"}) {
        osc_test::Builder named;
        named.begin(cmd) << 0 << "amp" << -1 << kHuge;
        fx.send(named.end());
        osc_test::Builder indexed;
        indexed.begin(cmd) << 0 << 0 << -1 << kHuge;
        fx.send(indexed.end());
        stillRunning(fx);
    }
    CHECK(controlOf(fx, 1009, "amp") == 0.5f);

    fx.send(osc_test::message("/g_freeAll", 0));
}

TEST_CASE("OSC counts: a mapping's bus number past int32 maps nothing", "[osc][counts]") {
    EngineFixture fx;
    REQUIRE(fx.loadSynthDef("sonic-pi-beep"));
    fx.send(sNew(1000));
    osc_test::Builder b;
    b.begin("/n_mapn") << 1000 << 0 << kHuge << 4;
    fx.send(b.end());
    stillRunning(fx);
    fx.send(osc_test::message("/n_free", 1000));
}

TEST_CASE("OSC counts: /s_getn refuses a count that is negative or past the synth's controls",
          "[osc][counts]") {
    EngineFixture fx;
    REQUIRE(fx.loadSynthDef("sonic-pi-beep"));
    fx.send(sNew(1000));

    for (const int32_t n : {-1, -50, -100, kHuge}) {
        osc_test::Builder b;
        b.begin("/s_getn") << 1000 << 0 << n;
        refused(fx, b.end(), "/s_getn");
    }

    osc_test::Builder ok;
    ok.begin("/s_getn") << 1000 << "amp" << 1;
    fx.clearReplies();
    fx.send(ok.end());
    OscReply r;
    REQUIRE(fx.waitForReply("/n_setn", r));
    CHECK(r.parsed().argInt(2) == 1);
    stillRunning(fx);
    fx.send(osc_test::message("/n_free", 1000));
}

TEST_CASE("OSC counts: /c_getn and /b_getn refuse a negative count", "[osc][counts]") {
    EngineFixture fx;
    fx.send(osc_test::message("/b_alloc", 0, 16, 1));
    OscReply r;
    REQUIRE(fx.waitForReply("/done", r));

    for (const int32_t n : {-1, -13, -50, -100}) {
        refused(fx, osc_test::message("/c_getn", 0, n), "/c_getn");
        refused(fx, osc_test::message("/b_getn", 0, 0, n), "/b_getn");
    }
    stillRunning(fx);
}

TEST_CASE("OSC counts: fill and setn ranges past int32 write nothing", "[osc][counts]") {
    EngineFixture fx;
    fx.send(osc_test::message("/b_alloc", 0, 16, 1));
    OscReply r;
    REQUIRE(fx.waitForReply("/done", r));

    const int32_t big = 0x7FFFFFF0;
    {
        osc_test::Builder b;
        b.begin("/c_fill") << big << big << 0.5f;
        fx.send(b.end());
    }
    {
        osc_test::Builder b;
        b.begin("/b_fill") << 0 << big << big << 0.5f;
        fx.send(b.end());
    }
    {
        osc_test::Builder b;
        b.begin("/b_setn") << 0 << big << big << 0.5f;
        fx.send(b.end());
    }
    refused(fx, osc_test::message("/c_setn", big, big), "/c_setn");
    stillRunning(fx);
}

TEST_CASE("OSC counts: a run of controls from an index near int32's end sets nothing",
          "[osc][counts]") {
    EngineFixture fx;
    REQUIRE(fx.loadSynthDef("sonic-pi-beep"));
    fx.send(sNew(1000));
    const float amp = controlOf(fx, 1000, "amp");

    // Each run starts at INT32_MAX, so its second value is one past it.
    {
        osc_test::Builder b;
        b.begin("/n_set") << 1000 << kHuge << osc::BeginArray << 0.1f << 0.2f << osc::EndArray;
        fx.send(b.end());
    }
    {
        osc_test::Builder b;
        b.begin("/n_setn") << 1000 << kHuge << 2 << 0.1f << 0.2f;
        fx.send(b.end());
    }
    {
        osc_test::Builder b;
        b.begin("/s_new") << "sonic-pi-beep" << 1001 << 0 << 0
                          << kHuge << osc::BeginArray << 0.1f << 0.2f << osc::EndArray;
        fx.send(b.end());
    }
    {
        osc_test::Builder b;
        b.begin("/s_newargs") << "sonic-pi-beep" << 1002 << 0 << 0 << kHuge << 2 << 0.1f << 0.2f;
        fx.send(b.end());
    }
    stillRunning(fx);
    CHECK(controlOf(fx, 1000, "amp") == amp);
    CHECK(controlOf(fx, 1001, "amp") == amp);
    CHECK(controlOf(fx, 1002, "amp") == amp);

    osc_test::Builder get;
    get.begin("/s_getn") << 1000 << kHuge << 2;
    fx.clearReplies();
    fx.send(get.end());
    OscReply r;
    REQUIRE(fx.waitForReply("/n_setn", r));
    const auto values = r.parsed();
    CHECK(values.argInt(2) == 2);
    CHECK(values.argFloat(3) == 0.0f);
    CHECK(values.argFloat(4) == 0.0f);

    stillRunning(fx);
    fx.send(osc_test::message("/g_freeAll", 0));
}
