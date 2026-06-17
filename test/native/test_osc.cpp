/*
 * test_osc.cpp — the /osc/ subsystem (OscControl) end to end through a real
 * loopback UDP socket: the cue server forwards inbound external OSC as
 * /external-osc-cue, and scheduled outbound OSC (/schedule → EngineScheduler →
 * OscControl) is delivered to the target host:port. Only built when
 * SUPERSONIC_ENABLE_OSC is on.
 */
#include <catch2/catch_test_macros.hpp>

#include "EngineFixture.h"
#include "OscTestUtils.h"
#include "WallClock.h"

#include <juce_core/juce_core.h>
#include <chrono>

#ifndef _WIN32
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#ifdef SUPERSONIC_OSC

// True if this host can bind the IPv6 loopback. juce::DatagramSocket is IPv4-only
// so it can't be used to probe; use a raw socket (Unix only — Windows v6 coverage
// comes from the cross-platform Rust subsystem tests).
static bool ipv6_loopback_available() {
#ifdef _WIN32
    return false;
#else
    int fd = ::socket(AF_INET6, SOCK_DGRAM, 0);
    if (fd < 0) return false;
    sockaddr_in6 a{};
    a.sin6_family = AF_INET6;
    a.sin6_addr = in6addr_loopback;
    a.sin6_port = 0;
    const bool ok = ::bind(fd, reinterpret_cast<sockaddr*>(&a), sizeof(a)) == 0;
    ::close(fd);
    return ok;
#endif
}

// Build the wire form for a timed outbound OSC send:
//   /schedule <ntp> </osc/send <host> <port> <inner>>
// The scheduler re-ingests the inner /osc/send on time → the same OscControl
// dispatch an immediate /osc/send hits.
static osc_test::Packet scheduleOscSend(double ntp, const char* host, int port,
                                        const osc_test::Packet& inner) {
    osc_test::Builder send;
    send.begin("/osc/send")
        << host
        << static_cast<osc::int32>(port)
        << osc::Blob(inner.data.data(),
                     static_cast<osc::osc_bundle_element_size_t>(inner.size()));
    osc_test::Packet sendPkt = send.end();
    // int64 OSC timetag (the 'h' path Sonic Pi/WASM use), not a double.
    const uint64_t s = static_cast<uint64_t>(ntp);
    const uint64_t f = static_cast<uint64_t>((ntp - static_cast<double>(s)) * 4294967296.0);
    const osc::int64 timetag = static_cast<osc::int64>((s << 32) | f);
    osc_test::Builder sch;
    sch.begin("/schedule")
        << timetag
        << osc::Blob(sendPkt.data.data(),
                     static_cast<osc::osc_bundle_element_size_t>(sendPkt.size()));
    return sch.end();
}

// Inbound: an external OSC message arriving on the cue port is re-framed and
// pushed to subscribers as /external-osc-cue <ip> <port> <address> <args...>.
TEST_CASE("OSC cue server forwards inbound external OSC as a cue", "[osc]") {
    EngineFixture fx;

    // Pick a free UDP port, then hand it to the cue server (probe closes first).
    int cuePort = 0;
    {
        juce::DatagramSocket probe;
        REQUIRE(probe.bindToPort(0, "127.0.0.1"));
        cuePort = probe.getBoundPort();
    }

    osc_test::Builder cfg;
    cfg.begin("/osc/cue-server/config")
        << static_cast<osc::int32>(cuePort)   // port
        << static_cast<osc::int32>(1)         // loopback-restricted
        << static_cast<osc::int32>(1);        // cues-on
    fx.send(cfg.end());

    // External message we'll send to the cue port.
    osc_test::Builder ext;
    ext.begin("/hello") << static_cast<osc::int32>(42) << "world";
    osc_test::Packet extPkt = ext.end();

    juce::DatagramSocket sender;
    REQUIRE(sender.bindToPort(0, "127.0.0.1"));

    // The cue server binds on its recv thread, so resend until a cue comes back.
    OscReply r;
    const bool got = fx.pollUntil([&] {
        sender.write("127.0.0.1", cuePort,
                     extPkt.ptr(), static_cast<int>(extPkt.size()));
        return fx.waitForReply("/external-osc-cue", r, 50);
    }, 4000);
    REQUIRE(got);

    auto p = r.parsed();
    CHECK(p.argString(0) == "127.0.0.1");   // sender ip
    CHECK(p.argInt(1) > 0);                  // sender port
    CHECK(p.argString(2) == "/hello");       // original address
    CHECK(p.argInt(3) == 42);
    CHECK(p.argString(4) == "world");
}

// Outbound: a scheduled OSC message is delivered to the target host:port via
// the EngineScheduler → OscControl path (the cue server socket is used to send).
TEST_CASE("scheduled OSC out is delivered to the target host:port", "[osc]") {
    EngineFixture fx;

    // A listener the test owns and binds, so there's no port race.
    juce::DatagramSocket listener;
    REQUIRE(listener.bindToPort(0, "127.0.0.1"));
    const int targetPort = listener.getBoundPort();

    // Outbound uses the subsystem's own send socket, so no cue-server config is
    // needed here — just schedule and read.

    // Inner user OSC message to deliver.
    osc_test::Builder inner;
    inner.begin("/world") << static_cast<osc::int32>(7);
    osc_test::Packet innerPkt = inner.end();

    // /schedule <ntp (just past → due now)> </osc/send <host> <port> <inner>>.
    fx.send(scheduleOscSend(wallClockNTP() - 1.0, "127.0.0.1", targetPort, innerPkt));

    fx.waitForBlocks(5);   // let the scheduler drain + the dispatch thread send

    char buf[1024];
    juce::String senderIp;
    int senderPort = 0;
    bool got = false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(2000);
    while (std::chrono::steady_clock::now() < deadline) {
        if (listener.waitUntilReady(true, 50) == 1) {
            const int n = listener.read(buf, sizeof(buf), false, senderIp, senderPort);
            if (n > 0) {
                auto pr = osc_test::parseReply(reinterpret_cast<const uint8_t*>(buf),
                                               static_cast<uint32_t>(n));
                CHECK(pr.address == "/world");
                CHECK(pr.argInt(0) == 7);
                got = true;
                break;
            }
        }
    }
    REQUIRE(got);
}

// Self-loop: Sonic Pi's `use_osc "localhost", <cuePort>; osc "/x"` — the engine
// sends to its OWN cue port and must receive it back as a cue. "localhost"
// resolves in the cue socket's family, so it lands rather than going astray.
TEST_CASE("OSC self-loop: sending to our own cue port comes back as a cue", "[osc]") {
    EngineFixture fx;

    int cuePort = 0;
    {
        juce::DatagramSocket probe;
        REQUIRE(probe.bindToPort(0, "127.0.0.1"));
        cuePort = probe.getBoundPort();
    }
    osc_test::Builder cfg;
    cfg.begin("/osc/cue-server/config")
        << static_cast<osc::int32>(cuePort)
        << static_cast<osc::int32>(1)    // loopback
        << static_cast<osc::int32>(1);   // cues-on
    fx.send(cfg.end());
    fx.waitForBlocks(8);                  // let the cue server recv thread bind

    osc_test::Builder inner;
    inner.begin("/selfcue") << static_cast<osc::int32>(123);
    osc_test::Packet innerPkt = inner.end();

    // Schedule the engine to send to its OWN cue port (localhost:cuePort).
    // Re-schedule until the cue returns — the cue server binds asynchronously on
    // its recv thread, and each send is a one-shot due-now send.
    OscReply r;
    const bool got = fx.pollUntil([&] {
        // "localhost" — a hostname, resolved by the subsystem (IPv4 or IPv6).
        fx.send(scheduleOscSend(wallClockNTP() - 1.0, "localhost", cuePort, innerPkt));
        return fx.waitForReply("/external-osc-cue", r, 100);
    }, 4000);
    REQUIRE(got);
    auto p = r.parsed();
    CHECK(p.argString(2) == "/selfcue");   // original address survived the round-trip
    CHECK(p.argInt(3) == 123);
}

// IPv6 self-loop through the full engine path: the engine sends to ::1:<cuePort>
// and the IPv6 cue listener receives it back — exercises dual-stack send + recv
// end to end. Skipped if the box has no IPv6 loopback.
TEST_CASE("OSC self-loop works over IPv6 (::1)", "[osc]") {
    if (!ipv6_loopback_available()) { SUCCEED("no IPv6 loopback here"); return; }
    EngineFixture fx;

    int cuePort = 0;
    {
        juce::DatagramSocket probe;
        REQUIRE(probe.bindToPort(0, "127.0.0.1"));
        cuePort = probe.getBoundPort();
    }
    osc_test::Builder cfg;
    cfg.begin("/osc/cue-server/config")
        << static_cast<osc::int32>(cuePort)
        << static_cast<osc::int32>(1)    // loopback (binds 127.0.0.1 + ::1)
        << static_cast<osc::int32>(1);   // cues-on
    fx.send(cfg.end());
    fx.waitForBlocks(8);

    osc_test::Builder inner;
    inner.begin("/selfcue6") << static_cast<osc::int32>(66);
    osc_test::Packet innerPkt = inner.end();

    OscReply r;
    const bool got = fx.pollUntil([&] {
        // "::1" — IPv6 literal destination.
        fx.send(scheduleOscSend(wallClockNTP() - 1.0, "::1", cuePort, innerPkt));
        return fx.waitForReply("/external-osc-cue", r, 100);
    }, 4000);
    REQUIRE(got);
    auto p = r.parsed();
    CHECK(p.argString(2) == "/selfcue6");
    CHECK(p.argInt(3) == 66);
    // Sender IP came back as an IPv6 address (contains a colon).
    CHECK(p.argString(0).find(':') != std::string::npos);
}

// A large (but in-budget) scheduled OSC message is delivered, and an oversized
// one is rejected loudly (logged) without crashing — guards the scheduler-slot
// payload cap so an over-large message is never silently swallowed.
TEST_CASE("OSC out: large message delivered, engine survives", "[osc]") {
    EngineFixture fx;
    juce::DatagramSocket listener;
    REQUIRE(listener.bindToPort(0, "127.0.0.1"));
    listener.waitUntilReady(false, 50);
    const int targetPort = listener.getBoundPort();

    auto blobMsg = [](const char* addr, size_t blobBytes) {
        std::vector<uint8_t> blob(blobBytes, 0xAB);
        return osc_test::messageWithBlob(addr, blob.data(), blob.size());
    };
    auto schedule = [&](const osc_test::Packet& inner) {
        fx.send(scheduleOscSend(wallClockNTP() - 1.0, "127.0.0.1", targetPort, inner));
    };

    // A multi-KB inner. The scheduler payload limit is its own data pool
    // (kMaxPayload = SCHEDULER_DATA_POOL_SIZE), so this must be delivered.
    schedule(blobMsg("/big", 4000));
    fx.waitForBlocks(5);
    char buf[8192];
    juce::String ip; int fromPort = 0;
    bool gotBig = false;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(2000);
    while (std::chrono::steady_clock::now() < deadline) {
        if (listener.waitUntilReady(true, 50) == 1) {
            int n = listener.read(buf, sizeof(buf), false, ip, fromPort);
            if (n > 0) { CHECK(osc_test::parseReply(reinterpret_cast<uint8_t*>(buf), (uint32_t)n).address == "/big"); gotBig = true; break; }
        }
    }
    REQUIRE(gotBig);

    // (The scheduler-level oversize drop — payload > kMaxPayload = the data pool —
    // is covered as a unit test in test_event_scheduler.cpp; a >512KB message
    // can't be built/sent through the OSC test harness here.)

    // Engine still alive and serving OSC afterwards.
    OscReply r;
    int cuePort = 0;
    { juce::DatagramSocket probe; REQUIRE(probe.bindToPort(0, "127.0.0.1")); cuePort = probe.getBoundPort(); }
    osc_test::Builder cfg;
    cfg.begin("/osc/cue-server/config") << static_cast<osc::int32>(cuePort)
        << static_cast<osc::int32>(1) << static_cast<osc::int32>(1);
    fx.send(cfg.end());
    juce::DatagramSocket sender; REQUIRE(sender.bindToPort(0, "127.0.0.1"));
    const bool alive = fx.pollUntil([&] {
        auto m = osc_test::message("/alive");
        sender.write("127.0.0.1", cuePort, m.ptr(), static_cast<int>(m.size()));
        return fx.waitForReply("/external-osc-cue", r, 50);
    }, 3000);
    CHECK(alive);
}

#endif // SUPERSONIC_OSC
