/*
 * test_graphdef_leak.cpp — /d_free must actually free the synthdef
 *
 * A /d_recv (build) followed by /d_free (destroy, no live instances) must
 * release everything the build took. The destroy path used to write to a
 * never-drained fifo and free nothing, leaking the whole GraphDef; this guards
 * that regression.
 *
 * Measured by repetition rather than by counting allocations. The counters in
 * test_rt_alloc.cpp hook operator new and delete, which sees only what the C++
 * side allocates; an engine that holds its definitions in memory obtained any
 * other way would report zero allocations and pass a balance check without
 * having been tested at all. Loading and freeing the same definition many
 * times leaks visibly whatever the allocator underneath, which is the property
 * this file is actually about.
 */

#include "EngineFixture.h"
#include "OscTestUtils.h"
#include "rt_alloc.h"
#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#if defined(__APPLE__)
#include <mach/mach.h>
#endif
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

extern "C" {
    bool process_audio(double current_time, uint32_t active_output_channels,
                       uint32_t active_input_channels);
}

#if defined(_WIN32)
#  include <windows.h>
#  include <psapi.h>
#  pragma comment(lib, "psapi.lib")
#endif

namespace {

std::vector<uint8_t> readSynthDef(const char* name) {
    std::filesystem::path p =
        std::filesystem::path(CLOCKWORK_SYNTHDEFS_DIR) / (std::string(name) + ".scsyndef");
    std::ifstream f(p, std::ios::binary);
    return { std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>() };
}

/// Resident set size in kilobytes, or 0 where it cannot be read.
///
/// A coarse instrument, deliberately: it is immune to which allocator the
/// engine uses, and a leak of a whole synthdef repeated hundreds of times is
/// not a subtle signal.
///
/// Two implementations because /proc is Linux's. This read /proc/self/statm
/// unconditionally and returned 0 everywhere else — and the caller's
/// REQUIRE(before > 0) then turned "this platform cannot be measured" into a
/// test failure, which is the wrong verdict: the engine was never exercised.
long resident_kb() {
#if defined(__APPLE__)
    // mach's task_basic_info is the platform's own answer to the same
    // question. resident_size is bytes.
    mach_task_basic_info_data_t info{};
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO,
                  reinterpret_cast<task_info_t>(&info), &count) != KERN_SUCCESS)
        return 0;
    return static_cast<long>(info.resident_size / 1024);
#elif defined(_WIN32)
    PROCESS_MEMORY_COUNTERS pmc{};
    pmc.cb = sizeof(pmc);
    if (!GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc))) return 0;
    return static_cast<long>(pmc.WorkingSetSize / 1024);
#else
    std::ifstream f("/proc/self/statm");
    long total = 0, resident = 0;
    if (!(f >> total >> resident)) return 0;
    return resident * (sysconf(_SC_PAGESIZE) / 1024);
#endif
}

// Under AddressSanitizer the resident set is not the engine's: the allocator
// quarantines freed blocks and pads every allocation, so 400 rounds of
// alloc-and-free grow it by megabytes with nothing leaked. The suite's
// sanitizer job is for races and UB; the leak question is answered by the
// uninstrumented matrix.
#if defined(__SANITIZE_ADDRESS__)
#  define GRAPHDEF_LEAK_UNDER_ASAN 1
#elif defined(__has_feature)
#  if __has_feature(address_sanitizer)
#    define GRAPHDEF_LEAK_UNDER_ASAN 1
#  endif
#endif

void pump(int blocks, double& ntp) {
    constexpr double blockSecs = 128.0 / 48000.0;
    for (int i = 0; i < blocks; ++i) {
        process_audio(ntp, 2, 0);
        ntp += blockSecs;
    }
}

} // namespace

TEST_CASE("GraphDef: /d_free frees the def (no leak)", "[graphdef_leak]") {
#ifdef GRAPHDEF_LEAK_UNDER_ASAN
    SKIP("resident set is the sanitizer's under ASan, not the engine's");
#endif
    EngineFixture fx;
    auto bytes = readSynthDef("sonic-pi-beep");
    REQUIRE(!bytes.empty());

    fx.stopHeadlessDriver();
    double ntp = 3'000'000'000.0;
    pump(200, ntp); // settle lazy init

    // Warm up: the first few rounds touch pages that stay touched, so the
    // baseline is taken after the allocator has reached a steady state rather
    // than before it.
    for (int i = 0; i < 20; ++i) {
        fx.send(osc_test::messageWithBlob("/d_recv", bytes.data(), bytes.size()));
        fx.send(osc_test::message("/d_free", "sonic-pi-beep"));
        pump(4, ntp);
    }

    const long before = resident_kb();
    REQUIRE(before > 0);

    constexpr int kRounds = 400;
    for (int i = 0; i < kRounds; ++i) {
        fx.send(osc_test::messageWithBlob("/d_recv", bytes.data(), bytes.size()));
        fx.send(osc_test::message("/d_free", "sonic-pi-beep"));
        pump(4, ntp);
    }
    const long after = resident_kb();

    // sonic-pi-beep is tens of kilobytes built out; leaking it 400 times would
    // add megabytes. A few hundred kilobytes of ordinary allocator drift is
    // not a leak, and this is set to tell the two apart rather than to be
    // tight.
    const long growth = after - before;
    INFO("resident before=" << before << "kB after=" << after << "kB growth=" << growth << "kB");
    CHECK(growth < 2048);

}
