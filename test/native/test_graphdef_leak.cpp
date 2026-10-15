/*
 * test_graphdef_leak.cpp — /d_free must actually free the synthdef
 *
 * A /d_recv (build) followed by /d_free (destroy, no live instances) must
 * release everything the build took. The destroy path used to write to a
 * never-drained fifo and free nothing, leaking the whole GraphDef; this guards
 * that regression.
 *
 * Measured by the guest's own census. Everything a definition is built out
 * of comes from the guest's pool (dsp/scsynth/guest_heap.h: clockwork's
 * allocator over the guest's bulk memory), and the pool counts the bytes in
 * use, exactly, into the metrics (guest_pool_used_bytes). Load and free the
 * same definition many times: the count after has to be the count before.
 * A definition leaked once is tens of kilobytes over; leaked every round,
 * megabytes.
 *
 * Until 2026-10-15 this weighed the process instead — its resident set
 * before and after, against a 2 MB allowance — to be independent of whichever
 * allocator held the definition. The resident set is the process's, not the
 * definition's: with the pool's count flat to the byte across 400 rounds it
 * drifted by 1.5 MB in one run and 7 MB in the next, and failed on Windows at
 * 2132 kB. Now that the definition's memory is counted where it lives, the
 * count is the measure.
 */
#include "EngineFixture.h"
#include "OscTestUtils.h"
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace {

std::vector<uint8_t> readSynthDef(const char* name) {
    std::filesystem::path p =
        std::filesystem::path(CLOCKWORK_SYNTHDEFS_DIR) / (std::string(name) + ".scsyndef");
    std::ifstream f(p, std::ios::binary);
    return { std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>() };
}

} // namespace

TEST_CASE("GraphDef: /d_free frees the def (no leak)", "[graphdef_leak]") {
    EngineFixture fx;
    auto bytes = readSynthDef("sonic-pi-beep");
    REQUIRE(!bytes.empty());

    fx.pumpBlock(200);   // settle lazy init

    // A round is the def in and out again, paced by the engine's /synced as a
    // client's would be: each round's def is gone before the next begins.
    int32_t syncId = 0;
    const auto round = [&] {
        fx.send(osc_test::messageWithBlob("/d_recv", bytes.data(), bytes.size()));
        fx.send(osc_test::message("/d_free", "sonic-pi-beep"));
        fx.send(osc_test::message("/sync", ++syncId));
        OscReply synced;
        REQUIRE(fx.waitForReply("/synced", synced));
    };

    // Warm up: the first rounds may leave something behind once (a table
    // grown to fit, kept), so the baseline is taken after them.
    for (int i = 0; i < 20; ++i) round();

    const auto& m = fx.engine().getMetrics();
    const uint32_t before = m.guest_pool_used_bytes.load(std::memory_order_relaxed);
    REQUIRE(before > 0);   // the guest's state is in the pool: a 0 would mean it is not being counted

    constexpr int kRounds = 400;
    for (int i = 0; i < kRounds; ++i) round();
    const uint32_t after = m.guest_pool_used_bytes.load(std::memory_order_relaxed);

    INFO("the guest's pool held " << before << " bytes before " << kRounds << " rounds and " << after
         << " after: " << (after >= before ? "+" : "-")
         << (after >= before ? after - before : before - after) << " bytes");
    CHECK(after == before);
}
