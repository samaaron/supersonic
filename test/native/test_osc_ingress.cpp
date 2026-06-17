/*
 * test_osc_ingress.cpp — acceptance tests for the engine Router (OscIngress):
 * the single, modular, capability-gated routing primitive shared by every build
 * (native UDP, NIF, wasm OscChannel, esp SLIP all funnel through it).
 *
 * Pure unit tests on the no_std header — no engine, no transport. Encodes:
 *   - #1 routing: unclaimed/bundle → default (audio plane); registered control
 *         prefix → its handler; longest prefix wins.
 *   - #5 modularity / capability-gating: a prefix is reachable ONLY if a route
 *         was registered for it; not registering it (a capability the target
 *         lacks) makes that traffic fall through to the default. Adding an
 *         endpoint is one registerRoute; removing it is not registering.
 */
#include <catch2/catch_test_macros.hpp>

#include "src/OscIngress.h"
#include <cstring>

namespace {

// A sink records how many packets it received and the last payload length.
struct Recorder {
    int    calls   = 0;
    size_t lastLen = 0;
};

bool sink(void* ctx, const void* /*callCtx*/, const uint8_t* data, std::size_t len) {
    (void)data;
    auto* r = static_cast<Recorder*>(ctx);
    r->calls++;
    r->lastLen = len;
    return true;
}

// Ingest a NUL-terminated OSC address (len includes the terminator).
void ingestAddr(const OscIngress& ix, const char* addr) {
    ix.ingest(reinterpret_cast<const uint8_t*>(addr),
              std::strlen(addr) + 1, nullptr);
}

} // namespace

TEST_CASE("Router: unclaimed messages and bundles go to the default sink (audio plane)",
          "[ingress][router]") {
    OscIngress ix;
    Recorder def, link;
    ix.setDefault(&sink, &def);
    ix.registerRoute("/clock/", &sink, &link);

    ingestAddr(ix, "/s_new");                  // a plain scsynth message
    CHECK(def.calls == 1);
    CHECK(link.calls == 0);

    // A bundle is always the audio/scheduler plane's job — never a control route.
    const uint8_t bundle[16] = { '#','b','u','n','d','l','e','\0', 0,0,0,0,0,0,0,1 };
    ix.ingest(bundle, sizeof(bundle), nullptr);
    CHECK(def.calls == 2);
    CHECK(link.calls == 0);
}

TEST_CASE("Router: a registered control prefix peels off to its handler",
          "[ingress][router]") {
    OscIngress ix;
    Recorder def, link;
    ix.setDefault(&sink, &def);
    ix.registerRoute("/clock/", &sink, &link);

    ingestAddr(ix, "/clock/tempo/get");
    CHECK(link.calls == 1);
    CHECK(def.calls == 0);

    // A sibling prefix that ISN'T registered falls through to the default —
    // this is capability-gating: a target without the /supersonic subsystem
    // simply never registers it, and that traffic is treated as audio.
    ingestAddr(ix, "/supersonic/devices/list");
    CHECK(def.calls == 1);
    CHECK(link.calls == 1);
}

TEST_CASE("Router: the longest registered prefix wins", "[ingress][router]") {
    OscIngress ix;
    Recorder def, a, ab;
    ix.setDefault(&sink, &def);
    ix.registerRoute("/a/",   &sink, &a);
    ix.registerRoute("/a/b/", &sink, &ab);

    ingestAddr(ix, "/a/x");      // only /a/ matches
    CHECK(a.calls == 1);
    CHECK(ab.calls == 0);

    ingestAddr(ix, "/a/b/x");    // /a/b/ is longer → wins over /a/
    CHECK(ab.calls == 1);
    CHECK(a.calls == 1);
}

TEST_CASE("Router is modular: a prefix is reachable only once its route is registered",
          "[ingress][router][capability]") {
    OscIngress ix;
    Recorder def, link;
    ix.setDefault(&sink, &def);

    // Capability absent (route not registered): /clock/ traffic is just audio.
    REQUIRE(ix.routeCount() == 0);
    ingestAddr(ix, "/clock/tempo/get");
    CHECK(def.calls == 1);
    CHECK(link.calls == 0);

    // Register the endpoint (one call) — the capability is now present.
    REQUIRE(ix.registerRoute("/clock/", &sink, &link));
    REQUIRE(ix.routeCount() == 1);

    // Same traffic now reaches the handler instead of the audio plane.
    ingestAddr(ix, "/clock/tempo/get");
    CHECK(link.calls == 1);
    CHECK(def.calls == 1);
}

TEST_CASE("Router drops non-OSC packets without dispatching", "[ingress][router]") {
    OscIngress ix;
    Recorder def;
    ix.setDefault(&sink, &def);

    ingestAddr(ix, "status");             // no leading '/'
    const uint8_t tiny[2] = { '/', 0 };   // too short to be an address
    ix.ingest(tiny, sizeof(tiny), nullptr);

    CHECK(def.calls == 0);
}
