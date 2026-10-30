// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Sam Aaron
/*
 * test_embedded_host.cpp — SuperSonic in an application's process
 * (host/EmbeddedHost.h): the engine and the front composed with no sockets,
 * every reply on one callback. What Sonic Pi's desktop app links.
 */
#include <catch2/catch_test_macros.hpp>

#include "EmbeddedHost.h"
#include "supersonic_commands.h"
#include "OscTestUtils.h"
#include "scsynth_options_text.h"

#include <chrono>
#include <condition_variable>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

// The headless config the engine fixture boots with (EngineFixture.cpp).
ClockworkEngine::Config headless() {
    ClockworkEngine::Config cfg;
    cfg.shmSegment           = false;
    cfg.headless          = true;
    cfg.numOutputChannels = 2;
    cfg.numInputChannels  = 0;
    cfg.sampleRate        = 48000;
    return cfg;
}

struct Replies {
    std::mutex mut;
    std::condition_variable cv;
    std::vector<std::vector<uint8_t>> all;

    void add(const uint8_t* d, uint32_t n) {
        { std::lock_guard<std::mutex> lk(mut); all.emplace_back(d, d + n); }
        cv.notify_all();
    }
    // A reply whose address is `addr`, within the timeout.
    bool waitFor(const char* addr, int timeoutMs = 3000) {
        std::unique_lock<std::mutex> lk(mut);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
        for (;;) {
            for (const auto& r : all)
                if (r.size() > std::strlen(addr) && std::memcmp(r.data(), addr, std::strlen(addr) + 1) == 0) return true;
            if (cv.wait_until(lk, deadline) == std::cv_status::timeout) return false;
        }
    }
};

} // namespace

TEST_CASE("embedded host: boots headless, answers /status on the callback, shuts down", "[embedded]") {
    supersonic::EmbeddedHost host;
    Replies replies;
    std::string err;
    supersonic::EmbeddedHost::Callbacks cb;
    cb.onMessage = [&](const uint8_t* d, uint32_t n) { replies.add(d, n); };
    cb.onLog = [](const std::string&) {};
    REQUIRE(host.boot(headless(), cb, &err));
    INFO(err);
    REQUIRE(host.running());
    CHECK(!host.summary().empty());

    const auto status = osc_test::message("/status");
    REQUIRE(host.send(status.ptr(), status.size()));
    CHECK(replies.waitFor("/status.reply"));

    // no segment was asked for (shmSegment false): no handle
    CHECK(!detail_shm_segment::shm_handle_valid(host.dupSegmentHandle()));

    host.shutdown();
    CHECK(!host.running());
    CHECK(!host.send(status.ptr(), status.size()));
    host.shutdown();   // idempotent
}

TEST_CASE("embedded host: the front answers a file verb for nothing with scsynth's /fail", "[embedded]") {
    supersonic::EmbeddedHost host;
    Replies replies;
    std::string err;
    supersonic::EmbeddedHost::Callbacks cb;
    cb.onMessage = [&](const uint8_t* d, uint32_t n) { replies.add(d, n); };
    REQUIRE(host.boot(headless(), cb, &err));
    osc_test::Builder b;
    b.begin("/b_allocRead") << 10 << "/no/such/file.wav" << 0 << 0;
    const auto load = b.end();
    REQUIRE(host.send(load.ptr(), load.size()));
    CHECK(replies.waitFor("/fail"));
    host.shutdown();
}

TEST_CASE("embedded host: a segment when shmSegment asks for one", "[embedded]") {
    supersonic::EmbeddedHost host;
    std::string err;
    auto cfg = headless();
    cfg.shmSegment = true;   // the segment's identity; nothing is bound
    REQUIRE(host.boot(cfg, {}, &err));
    const auto h = host.dupSegmentHandle();
    REQUIRE(detail_shm_segment::shm_handle_valid(h));
    shm_segment_client reader(h);   // owns the duplicate from here
    CHECK(reader.get_window_size() > 0);
    host.shutdown();
}

TEST_CASE("scsynth options as a struct round-trip through the guest's text", "[embedded][options]") {
    ScsynthOptions o;
    scsynth_options_defaults(&o);
    o.numAudioBusChannels = 1024;
    o.realTimeMemorySize = 131072;
    o.maxNodes = 4096;
    const std::string text = scsynth_options_text(o);
    CHECK(text.find("numAudioBusChannels=1024\n") != std::string::npos);
    CHECK(text.find("realTimeMemorySize=131072\n") != std::string::npos);

    ScsynthOptions back;
    char errBuf[256];
    REQUIRE(scsynth_options_parse(text.c_str(), static_cast<uint32_t>(text.size()), &back, errBuf, sizeof errBuf) == 0);
    CHECK(back.numAudioBusChannels == 1024);
    CHECK(back.realTimeMemorySize == 131072);
    CHECK(back.maxNodes == 4096);
    CHECK(back.numBuffers == o.numBuffers);
}

#ifndef CLOCKWORK_SAMPLES_DIR
#define CLOCKWORK_SAMPLES_DIR ""
#endif

TEST_CASE("embedded host: a sample loaded before a rebuild is the new instance's, and freeing it gives its slot back",
          "[embedded][ColdSwap][load_sample]") {
    // The whole path, with nothing called by hand: the engine rebuilds and hands the new instance every asset the
    // old one held (clockwork's docs/RESTARTS.md), so the sample's slot in the lane stays held, the new World plays
    // from it, and freeing the buffer there gives the slot back.
    const std::string sample = std::string(CLOCKWORK_SAMPLES_DIR) + "/bd_haus.flac";
    if (!std::filesystem::exists(sample)) SKIP("sample bd_haus.flac not found");
    supersonic::EmbeddedHost host;
    Replies replies;
    std::string err;
    supersonic::EmbeddedHost::Callbacks cb;
    cb.onMessage = [&](const uint8_t* d, uint32_t n) { replies.add(d, n); };
    REQUIRE(host.boot(headless(), cb, &err));

    osc_test::Builder b;
    b.begin("/b_allocRead") << int32_t{0} << sample.c_str() << int32_t{0} << int32_t{0};
    const auto load = b.end();
    REQUIRE(host.send(load.ptr(), load.size()));
    REQUIRE(replies.waitFor("/done"));
    const uint32_t held = host.front().laneFreeBytes();
    REQUIRE(held < host.front().laneBytes());

    const auto result = host.engine().switchDevice("", 44100);
    REQUIRE(result.success);
    REQUIRE(result.type == SwapType::Cold);
    CHECK(host.front().laneFreeBytes() == held);

    const auto query = osc_test::message("/b_query", int32_t{0});
    REQUIRE(host.send(query.ptr(), query.size()));
    REQUIRE(replies.waitFor("/b_info"));
    int32_t frames = 0;
    {
        std::lock_guard<std::mutex> lk(replies.mut);
        for (const auto& r : replies.all) {
            const auto p = osc_test::parseReply(r.data(), static_cast<uint32_t>(r.size()));
            if (p.address == "/b_info" && p.argInt(0) == 0) frames = p.argInt(1);
        }
    }
    CHECK(frames > 0);

    const auto free = osc_test::message("/b_free", int32_t{0});
    REQUIRE(host.send(free.ptr(), free.size()));
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (host.front().laneFreeBytes() != host.front().laneBytes()
           && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    CHECK(host.front().laneFreeBytes() == host.front().laneBytes());
    host.shutdown();
}
