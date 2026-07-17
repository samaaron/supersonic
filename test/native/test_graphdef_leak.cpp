/*
 * test_graphdef_leak.cpp — /d_free must actually free the synthdef
 *
 * A /d_recv (build) followed by /d_free (destroy, no live instances) must leave
 * global-heap allocations and frees balanced. The destroy path used to write to
 * a never-drained fifo and free nothing, leaking the whole GraphDef; this guards
 * that regression. Uses the operator new/delete counters from test_rt_alloc.cpp.
 */

#include "EngineFixture.h"
#include "OscTestUtils.h"
#include "rt_alloc.h"
#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

extern "C" {
    bool process_audio(double current_time, uint32_t active_output_channels,
                       uint32_t active_input_channels);
}

namespace {

std::vector<uint8_t> readSynthDef(const char* name) {
    std::filesystem::path p =
        std::filesystem::path(SUPERSONIC_SYNTHDEFS_DIR) / (std::string(name) + ".scsyndef");
    std::ifstream f(p, std::ios::binary);
    return { std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>() };
}

void pump(int blocks, double& ntp) {
    constexpr double blockSecs = 128.0 / 48000.0;
    for (int i = 0; i < blocks; ++i) {
        process_audio(ntp, 2, 0);
        ntp += blockSecs;
    }
}

} // namespace

TEST_CASE("GraphDef: /d_free frees the def (no leak)", "[graphdef_leak]") {
#if defined(RT_ALLOC_HOOKS_UNAVAILABLE)
    SKIP("needs the operator new/delete counters from test_rt_alloc.cpp, "
         "which cannot link under TSan (see rt_alloc.h)");
#else
    EngineFixture fx;
    auto bytes = readSynthDef("sonic-pi-beep");
    REQUIRE(!bytes.empty());

    fx.stopHeadlessDriver();
    double ntp = 3'000'000'000.0;
    pump(200, ntp); // settle lazy init

    // Build then destroy the def with no live instances, both drained inside the
    // guard: the destroy must return every allocation the build made.
    fx.send(osc_test::messageWithBlob("/d_recv", bytes.data(), bytes.size()));
    fx.send(osc_test::message("/d_free", "sonic-pi-beep"));

    rt_alloc::reset();
    {
        rt_alloc::Guard g;
        pump(200, ntp);
    }
    int64_t allocs = rt_alloc::g_allocs.load(std::memory_order_relaxed);
    int64_t frees = rt_alloc::g_frees.load(std::memory_order_relaxed);
    INFO("allocs=" << allocs << " frees=" << frees);
    CHECK(allocs > 0);        // the def actually built under the guard
    CHECK(allocs == frees);   // and every build allocation was freed by the destroy
#endif  // !RT_ALLOC_HOOKS_UNAVAILABLE
}
