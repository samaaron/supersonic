/*
 * test_prescheduler_core.cpp — drives the portable PreschedulerCore against
 * test/vectors/prescheduler.json, the same language-neutral vectors the JS core
 * runs (test/prescheduler_core.spec.mjs). A green run on both sides is what
 * holds the two implementations to one behaviour.
 *
 * Event-driven sim: a logical clock in seconds advances to each op's t, applies
 * it, and between/after ops releases everything due — a past-due event fires at
 * the current clock (delay clamped to 0), never running time backward.
 */
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include <juce_core/juce_core.h>

#include "src/workers/PreschedulerCore.h"

using namespace supersonic;

namespace {

double round6(double x) { return std::round(x * 1e6) / 1e6; }

const char* reasonString(PreschedulerStatus s) {
    switch (s) {
        case PreschedulerStatus::RejectQueueFull:    return "queue_full";
        case PreschedulerStatus::RejectTooLarge:     return "too_large";
        case PreschedulerStatus::RejectTooFarFuture: return "too_far_future";
        default:                                     return "ok";
    }
}

// Everything the sink and ops record, plus the live clock the sink stamps with.
struct SimState {
    std::vector<std::string>     dispatched;
    std::map<std::string, double> releasedAt;
    double                       now = 0.0;
};

void recordDispatch(SimState& st, const char* id) {
    st.dispatched.emplace_back(id);
    st.releasedAt[id] = round6(st.now);
}

void sink(const PreschedulerEvent& ev, void* ctx) {
    auto& st = *static_cast<SimState*>(ctx);
    recordDispatch(st, static_cast<const char*>(ev.payload));
}

struct Rejected {
    std::string id;
    std::string reason;
};

struct SimResult {
    std::vector<std::string>      dispatched;
    std::map<std::string, double> releasedAt;
    std::vector<Rejected>         rejected;
    int                           cancelled = 0;
};

double cfgNum(const juce::var& v, const char* key, const juce::var& defaults) {
    if (v.hasProperty(key)) return static_cast<double>(v[key]);
    return static_cast<double>(defaults[key]);
}

SimResult runVector(const juce::var& v, const juce::var& defaults) {
    PreschedulerConfig cfg;
    cfg.lookaheadS = cfgNum(v, "lookahead_s", defaults);
    cfg.maxPending = static_cast<uint32_t>(cfgNum(v, "max_pending", defaults));
    cfg.maxFutureS = cfgNum(v, "max_future_s", defaults);
    cfg.poolBytes  = static_cast<uint32_t>(cfgNum(v, "pool_bytes", defaults));

    // Capacity comfortably exceeds any vector's pending set; maxPending (which
    // the backpressure vector drops to 2) is the policy the core enforces.
    std::vector<PreschedulerEvent> storage(1024);
    PreschedulerCore core(storage.data(), static_cast<uint32_t>(storage.size()), cfg);

    // Payload ids must outlive dispatch; stash them stably.
    std::vector<std::string> idPool;
    idPool.reserve(256);
    auto internId = [&](const juce::String& s) -> const char* {
        idPool.emplace_back(s.toStdString());
        return idPool.back().c_str();
    };

    SimResult res;
    SimState st;

    // Release everything due at or before `limit`, advancing the clock to each
    // event's release time (never backward).
    auto drainDueUpTo = [&](double limit) {
        double due;
        while (core.nextDueTime(&due) && due <= limit) {
            st.now = std::max(st.now, due);
            core.dispatchDue(st.now, sink, &st);
        }
    };

    juce::var opsVar = v["ops"];  // hold a ref so getArray() stays valid through the loop
    for (const auto& op : *opsVar.getArray()) {
        const double t = static_cast<double>(op["t"]);
        drainDueUpTo(t);
        st.now = std::max(st.now, t);

        if (op.hasProperty("schedule")) {
            const juce::var& s = op["schedule"];
            const char* id = internId(s["id"].toString());
            PreschedulerRequest req;
            req.hasTime   = s.hasProperty("due");
            req.ntpTime   = req.hasTime ? static_cast<double>(s["due"]) : 0.0;
            req.bytes     = s.hasProperty("bytes") ? static_cast<uint32_t>(static_cast<int>(s["bytes"])) : 0;
            req.sessionId = s.hasProperty("session") ? static_cast<uint32_t>(static_cast<int>(s["session"])) : 0;
            req.tagId     = PreschedulerTagHash(s.hasProperty("tag") ? s["tag"].toString().toRawUTF8() : "");
            req.payload   = const_cast<char*>(id);
            PreschedulerStatus status = core.schedule(req, st.now);
            if (status != PreschedulerStatus::Scheduled && status != PreschedulerStatus::Immediate)
                res.rejected.push_back({ id, reasonString(status) });
        } else if (op.hasProperty("send")) {
            const char* id = internId(op["send"]["id"].toString());
            PreschedulerRequest req;
            req.hasTime = false;
            req.payload = const_cast<char*>(id);
            if (core.schedule(req, st.now) == PreschedulerStatus::Immediate)
                recordDispatch(st, id);
        } else if (op.hasProperty("cancelTag")) {
            res.cancelled += core.cancelTag(PreschedulerTagHash(op["cancelTag"].toString().toRawUTF8()));
        } else if (op.hasProperty("cancelSession")) {
            res.cancelled += core.cancelSession(static_cast<uint32_t>(static_cast<int>(op["cancelSession"])));
        } else if (op.hasProperty("cancelSessionTag")) {
            const juce::var& cs = op["cancelSessionTag"];
            res.cancelled += core.cancelSessionTag(
                static_cast<uint32_t>(static_cast<int>(cs["session"])),
                PreschedulerTagHash(cs["tag"].toString().toRawUTF8()));
        } else if (op.hasProperty("cancelAll")) {
            res.cancelled += core.cancelAll();
        }
    }

    double due;
    while (core.nextDueTime(&due)) {
        st.now = std::max(st.now, due);
        core.dispatchDue(st.now, sink, &st);
    }

    res.dispatched = st.dispatched;
    res.releasedAt = st.releasedAt;
    return res;
}

} // namespace

TEST_CASE("PreschedulerCore matches the shared vectors", "[prescheduler]") {
    juce::File vectorsFile(SUPERSONIC_PRESCHEDULER_VECTORS);
    REQUIRE(vectorsFile.existsAsFile());

    juce::var root = juce::JSON::parse(vectorsFile.loadFileAsString());
    REQUIRE(root.isObject());

    const juce::var defaults = root["defaults"];
    const juce::var vectorsVar = root["vectors"];
    REQUIRE(vectorsVar.isArray());

    for (const auto& v : *vectorsVar.getArray()) {
        const std::string name = v["name"].toString().toStdString();
        const juce::var expect = v["expect"];
        SimResult got = runVector(v, defaults);
        INFO("vector: " << name);

        // dispatched: exact order.
        if (expect.hasProperty("dispatched")) {
            std::vector<std::string> exp;
            for (const auto& e : *expect["dispatched"].getArray())
                exp.push_back(e.toString().toStdString());
            CHECK(got.dispatched == exp);
        }

        // cancelled: count.
        if (expect.hasProperty("cancelled"))
            CHECK(got.cancelled == static_cast<int>(expect["cancelled"]));

        // rejected: set of {id, reason}, order-independent.
        if (expect.hasProperty("rejected")) {
            std::map<std::string, std::string> expMap, gotMap;
            for (const auto& r : *expect["rejected"].getArray())
                expMap[r["id"].toString().toStdString()] = r["reason"].toString().toStdString();
            for (const auto& r : got.rejected) gotMap[r.id] = r.reason;
            CHECK(gotMap == expMap);
        }

        // released_at: per-id release time.
        if (expect.hasProperty("released_at")) {
            const juce::DynamicObject* ra = expect["released_at"].getDynamicObject();
            REQUIRE(ra != nullptr);
            for (const auto& prop : ra->getProperties()) {
                const std::string id = prop.name.toString().toStdString();
                const double want = static_cast<double>(prop.value);
                REQUIRE(got.releasedAt.count(id) == 1);
                CHECK(got.releasedAt[id] == Catch::Approx(want));
            }
        }
    }
}

// ── Differential fuzz: core vs. an independent reference model ───────────────
//
// The shared vectors prove the documented behaviours but only at ≤3 events, so
// they don't stress the heap's sift/heapify/tie-break at depth, nor the
// onRemoved reclamation callback (vital for the embedded payload pool). This
// pits the core against a dead-simple reference (a flat list sorted by
// (ntpTime, seq)) over thousands of randomized schedule/cancel/drain ops.
namespace {

struct RefEvent {
    double   ntpTime;
    uint32_t seq;
    uint32_t sessionId;
    uint64_t tagId;
    int      id;
};

// Deterministic LCG — no wall-clock/random dependency, fully reproducible.
struct Lcg {
    uint64_t s;
    explicit Lcg(uint64_t seed) : s(seed) {}
    uint32_t next() { s = s * 6364136223846793005ull + 1442695040888963407ull; return uint32_t(s >> 33); }
    uint32_t below(uint32_t n) { return next() % n; }
};

// Sinks collect the int ids packed into payload.
void collectId(const PreschedulerEvent& ev, void* ctx) {
    static_cast<std::vector<int>*>(ctx)->push_back(static_cast<int>(reinterpret_cast<uintptr_t>(ev.payload)));
}

} // namespace

TEST_CASE("PreschedulerCore matches a reference model under randomized ops", "[prescheduler]") {
    const uint64_t kTags[4] = {
        PreschedulerTagHash("t0"), PreschedulerTagHash("t1"),
        PreschedulerTagHash("t2"), PreschedulerTagHash("t3"),
    };

    for (int trial = 0; trial < 300; ++trial) {
        Lcg rng(0x1234567ull + uint64_t(trial) * 0x9E3779B97F4A7C15ull);

        PreschedulerConfig cfg;
        cfg.lookaheadS = 0.0;     // dispatchDue(now=huge) then releases the whole heap
        cfg.maxPending = 2048;
        cfg.maxFutureS = 1e12;
        cfg.poolBytes  = 1u << 20;
        std::vector<PreschedulerEvent> storage(2048);
        PreschedulerCore core(storage.data(), uint32_t(storage.size()), cfg);

        std::vector<RefEvent> ref;
        uint32_t seq = 0;
        int nextId = 0;
        const uint32_t opCount = 1 + rng.below(600);

        for (uint32_t op = 0; op < opCount; ++op) {
            const uint32_t roll = rng.below(100);
            if (roll < 75) {
                // schedule: small tag/session space so cancels actually hit; many
                // duplicate ntpTimes to exercise the seq tie-break.
                RefEvent e;
                e.ntpTime   = double(rng.below(50));
                e.sessionId = rng.below(4);
                e.tagId     = kTags[rng.below(4)];
                e.id        = nextId++;

                PreschedulerRequest req;
                req.hasTime   = true;
                req.ntpTime   = e.ntpTime;
                req.bytes     = 16;
                req.sessionId = e.sessionId;
                req.tagId     = e.tagId;
                req.payload   = reinterpret_cast<void*>(uintptr_t(e.id));

                PreschedulerStatus st = core.schedule(req, 0.0);
                // capacity (2048) ≥ ref size by construction, so it always queues
                REQUIRE(st == PreschedulerStatus::Scheduled);
                e.seq = seq++;
                ref.push_back(e);
            } else {
                // cancel by tag / session / (session,tag) / all — onRemoved must
                // report exactly the events the reference removes.
                const uint32_t kind = rng.below(4);
                std::vector<int> removed;
                uint32_t coreCount = 0;
                auto matches = [&](const RefEvent& e, uint64_t tag, uint32_t session) {
                    switch (kind) {
                        case 0: return e.tagId == tag;
                        case 1: return e.sessionId == session;
                        case 2: return e.sessionId == session && e.tagId == tag;
                        default: return true;
                    }
                };
                const uint64_t tag = kTags[rng.below(4)];
                const uint32_t session = rng.below(4);

                switch (kind) {
                    case 0: coreCount = core.cancelTag(tag, collectId, &removed); break;
                    case 1: coreCount = core.cancelSession(session, collectId, &removed); break;
                    case 2: coreCount = core.cancelSessionTag(session, tag, collectId, &removed); break;
                    default: coreCount = core.cancelAll(collectId, &removed); break;
                }

                std::vector<int> refRemoved;
                std::vector<RefEvent> survivors;
                for (const auto& e : ref) {
                    if (matches(e, tag, session)) refRemoved.push_back(e.id);
                    else survivors.push_back(e);
                }
                ref.swap(survivors);

                // onRemoved fired once per removed event, and the count agrees.
                REQUIRE(coreCount == refRemoved.size());
                std::sort(removed.begin(), removed.end());
                std::sort(refRemoved.begin(), refRemoved.end());
                REQUIRE(removed == refRemoved);
                REQUIRE(core.size() == ref.size());
            }
        }

        // Drain everything and compare dispatch order against the reference
        // sorted by (ntpTime, seq) — the FIFO-tie contract, at depth.
        std::vector<int> got;
        core.dispatchDue(1e12, collectId, &got);
        REQUIRE(core.size() == 0);

        std::stable_sort(ref.begin(), ref.end(), [](const RefEvent& a, const RefEvent& b) {
            if (a.ntpTime != b.ntpTime) return a.ntpTime < b.ntpTime;
            return a.seq < b.seq;
        });
        std::vector<int> want;
        want.reserve(ref.size());
        for (const auto& e : ref) want.push_back(e.id);

        INFO("trial " << trial);
        REQUIRE(got == want);
    }
}
