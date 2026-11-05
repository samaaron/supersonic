// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Sam Aaron
/*
 * test_fixture_engines.cpp — the fixture's engines beside the first.
 *
 * SUPERSONIC_TEST_ENGINES=n runs every pumped case on n engines at once in
 * this process (EngineFixture.h): the same program to each, and each held to
 * the first, block for block and reply for reply. These cases hold the
 * fixture to that: a message reaches every engine and each answers on its
 * own; engines told the same thing render the same bytes; and an engine told
 * something the others were not is caught, at the block it differs and in the
 * reply it gives.
 */
#include "EngineFixture.h"
#include "OscTestUtils.h"
#include <catch2/catch_test_macros.hpp>
#include <cstring>
#include <filesystem>
#include <string>

namespace {

int count(const std::vector<OscReply>& replies, const std::string& address) {
    int n = 0;
    for (const OscReply& r : replies)
        if (r.address == address) ++n;
    return n;
}

osc_test::Packet beep(float note) {
    osc_test::Builder b;
    b.begin("/s_new") << "sonic-pi-beep" << int32_t{1000} << int32_t{0} << int32_t{0} << "note" << note;
    return b.end();
}

bool beepAvailable() {
    return std::filesystem::exists(std::string(CLOCKWORK_SYNTHDEFS_DIR) + "/sonic-pi-beep.scsyndef");
}

} // namespace

TEST_CASE("fixture engines: a message reaches every engine and each answers on its own", "[fixture][plural]") {
    EngineFixture fx(3);
    REQUIRE(fx.engines() == 3);
    fx.send(osc_test::message("/status"));
    OscReply r;
    REQUIRE(fx.waitForReply("/status.reply", r));
    for (int engine = 0; engine < 3; ++engine) {
        INFO("engine " << engine + 1);
        CHECK(count(fx.repliesOf(engine), "/status.reply") == 1);
    }
}

TEST_CASE("fixture engines: engines told the same program render the same bytes", "[fixture][plural]") {
    if (!beepAvailable()) SKIP("sonic-pi-beep synthdef not available");
    EngineFixture fx(3);
    REQUIRE(fx.loadSynthDef("sonic-pi-beep"));
    fx.send(beep(60.0f));
    fx.pumpBlock(200);
    CHECK_FALSE(fx.divergence());
    CHECK_FALSE(fx.replyDivergence());
}

TEST_CASE("fixture engines: an engine told what the others were not is caught at the block it differs", "[fixture][plural]") {
    if (!beepAvailable()) SKIP("sonic-pi-beep synthdef not available");
    EngineFixture fx(3);
    fx.expectDivergence();
    REQUIRE(fx.loadSynthDef("sonic-pi-beep"));
    fx.sendToFirst(beep(60.0f));   // the first engine alone
    fx.pumpBlock(50);
    const auto d = fx.divergence();
    REQUIRE(d);
    CHECK(d->engine >= 1);
    CHECK(d->block <= 50);
    CHECK(std::memcmp(&d->got, &d->want, sizeof(float)) != 0);   // bit for bit: a -0 is a difference
}

TEST_CASE("fixture engines: an engine that answers what the others did not is caught in its replies", "[fixture][plural]") {
    EngineFixture fx(3);
    fx.expectDivergence();
    fx.sendToFirst(osc_test::message("/status"));   // the first engine alone
    OscReply r;
    REQUIRE(fx.waitForReply("/status.reply", r));
    const auto differs = fx.replyDivergence();
    REQUIRE(differs);
    CHECK(differs->find("engine 2") != std::string::npos);
    CHECK(differs->find("answered 0 replies") != std::string::npos);
}
