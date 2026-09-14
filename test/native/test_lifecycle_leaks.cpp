/*
 * test_lifecycle_leaks.cpp - Detect resource leaks across start/stop cycles.
 *
 * Drives 40 init/shutdown cycles in headless mode and asserts that file
 * descriptors and thread count return to baseline, and that the heap the
 * allocator has in use does not keep rising (see heapInUseBytes).
 *
 * Scope: catches FD/thread regressions in the graceful no-throw lifecycle.
 * Does NOT exercise partial-init failure (init() throwing partway),
 * which needs a separate test with a failure-injection hook on the engine.
 * Does NOT install the macOS CoreAudio property listener, since the test
 * runs in headless mode. A leak smaller than kHeapDriftBytes over twenty
 * cycles does not register.
 *
 * Linux-only: reads /proc/self. Test cases are compiled out on other
 * platforms.
 */
#include "ClockworkEngine.h"
#include <catch2/catch_test_macros.hpp>

#if defined(__linux__)

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <string>
#include <thread>
#include <vector>

// The heap is read from the allocator. Under AddressSanitizer malloc is the
// sanitizer's and glibc's counters read zero — the sanitizer job runs this
// test — so the sanitizer's own count is read there.
#if defined(__SANITIZE_ADDRESS__)
#  define LEAKS_UNDER_ASAN 1
#elif defined(__has_feature)
#  if __has_feature(address_sanitizer)
#    define LEAKS_UNDER_ASAN 1
#  endif
#endif
#if defined(LEAKS_UNDER_ASAN)
#  include <sanitizer/allocator_interface.h>
#else
#  include <malloc.h>
#endif

namespace {

// How far the heap's floor may rise across twenty cycles. Measured on macOS
// (2026-09-14, same engine, the system allocator's in-use count): the floor
// rose 25 KB across twenty cycles, identically on two runs — about 1.25 KB a
// boot. This allows forty times that, and fails a leak of 52 KB a cycle.
constexpr size_t kHeapDriftBytes = 1024 * 1024;

// Bytes the allocator has handed out and not been given back.
//
// Not resident memory. RSS also counts memory that was freed and that the
// allocator kept, and when glibc keeps another region is its own business: on
// 2026-09-14 the Debian build grew 0 KB over twenty engine cycles and 32 MB
// over the next twenty, with file descriptors and threads exactly back, and
// failed a README commit. In-use bytes rise only when something is allocated
// and not freed, which is what a leak is.
size_t heapInUseBytes() {
#if defined(LEAKS_UNDER_ASAN)
    return __sanitizer_get_current_allocated_bytes();
#elif defined(__GLIBC__) && __GLIBC_PREREQ(2, 33)
    const struct mallinfo2 mi = mallinfo2();
    return mi.uordblks + mi.hblkhd;
#else
    const struct mallinfo mi = mallinfo();
    return static_cast<size_t>(static_cast<unsigned>(mi.uordblks))
         + static_cast<size_t>(static_cast<unsigned>(mi.hblkhd));
#endif
}

size_t floorOver(const std::vector<size_t>& v, size_t from, size_t to) {
    return *std::min_element(v.begin() + from, v.begin() + to);
}

long readRssKb() {
    FILE* f = std::fopen("/proc/self/status", "r");
    if (!f) return -1;
    char line[256];
    long rss = -1;
    while (std::fgets(line, sizeof(line), f)) {
        if (std::strncmp(line, "VmRSS:", 6) == 0) {
            std::sscanf(line + 6, "%ld", &rss);
            break;
        }
    }
    std::fclose(f);
    return rss;
}

int countDirEntries(const char* path) {
    DIR* d = opendir(path);
    if (!d) return -1;
    int n = 0;
    while (auto* e = readdir(d)) {
        if (std::strcmp(e->d_name, ".") == 0 || std::strcmp(e->d_name, "..") == 0)
            continue;
        ++n;
    }
    closedir(d);
    return n;
}

int countFds()     { return countDirEntries("/proc/self/fd"); }
int countThreads() { return countDirEntries("/proc/self/task"); }

// JUCE threads on Linux are pthread_detach'd at creation
// (juce_SharedCode_posix.h). stopThread() returns once the dying thread
// clears threadHandle, before the kernel reaps /proc/self/task/<tid>.
// Reap delay is tens of ms typically, hundreds under load, so a thread
// count sampled right after shutdown can latch a zombie entry.
// WARN on timeout so a downstream assertion failure is distinguishable
// from a real leak.
int settleThreadCount(std::chrono::milliseconds timeout = std::chrono::milliseconds(3000)) {
    constexpr int kStableSamples = 3;        // consecutive identical samples
    constexpr auto kInterval = std::chrono::milliseconds(10);

    auto deadline = std::chrono::steady_clock::now() + timeout;
    int last = countThreads();
    int stableRun = 1;
    while (std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(kInterval);
        int now = countThreads();
        if (now == last) {
            if (++stableRun >= kStableSamples) return now;
        } else {
            last = now;
            stableRun = 1;
        }
    }
    WARN("settleThreadCount: /proc/self/task did not stabilise within "
         << timeout.count() << "ms (last=" << last
         << ", stableRun=" << stableRun << "/" << kStableSamples << ")");
    return last;
}

}  // namespace

TEST_CASE("Repeated init/shutdown does not leak FDs or threads", "[lifecycle][stress]") {
    constexpr int kCycles = 40;

    ClockworkEngine::Config cfg;
    cfg.headless = true;
    cfg.udpPort  = 0;

    // Warm-up cycle: first boot allocates one-shot caches (JUCE's device-type
    // list, sndfile lookup tables, etc.) that persist until process exit and
    // would otherwise be misread as a leak. Measure baseline AFTER warm-up.
    {
        ClockworkEngine engine;
        engine.init(cfg);
        REQUIRE(engine.isRunning());
        engine.shutdown();
    }

    const size_t baselineHeap    = heapInUseBytes();
    const long   baselineRss     = readRssKb();
    const int    baselineFds     = countFds();
    const int    baselineThreads = settleThreadCount();

    REQUIRE(baselineRss > 0);
    REQUIRE(baselineFds > 0);
    REQUIRE(baselineThreads > 0);

    // The heap, read after every cycle, once that cycle's engine is destroyed:
    // a shut-down engine still owns its arena until then, and the warm-up's
    // baseline is read the same way. A leak raises the heap's floor every
    // cycle; something still live at the moment of a reading (a thread's
    // cache, a buffer on its way to being freed) raises only that reading. So
    // the rule compares floors: the lowest reading in cycles 30-39 against the
    // lowest in cycles 10-19, twenty cycles apart. The first ten are left out
    // for caches that fill on first use. Both vectors are sized before the
    // first reading, so they add nothing to the heap they measure.
    std::vector<size_t> heap;
    std::vector<long>   rss;
    heap.reserve(kCycles);
    rss.reserve(kCycles);
    for (int i = 0; i < kCycles; ++i) {
        {
            ClockworkEngine engine;
            engine.init(cfg);
            REQUIRE(engine.isRunning());
            engine.shutdown();
            REQUIRE_FALSE(engine.isRunning());
        }
        heap.push_back(heapInUseBytes());
        rss.push_back(readRssKb());
    }

    const int finalFds     = countFds();
    const int finalThreads = settleThreadCount();

    const size_t early = floorOver(heap, 10, 20);
    const size_t late  = floorOver(heap, 30, 40);

    std::string series;
    for (int i = 0; i < kCycles; ++i) {
        series += std::to_string(i) + ":" + std::to_string(heap[i] / 1024) + "/"
                + std::to_string(rss[i]) + " ";
    }
    INFO("baseline heap=" << baselineHeap / 1024 << "kb rss=" << baselineRss
                          << "kb fds=" << baselineFds << " threads=" << baselineThreads);
    INFO("per cycle, heap in use kb / rss kb: " << series);
    INFO("heap floor, cycles 10-19: " << early / 1024 << "kb; cycles 30-39: "
                                      << late / 1024 << "kb; fds=" << finalFds
                                      << " threads=" << finalThreads);

    // FDs and threads must return to baseline exactly.
    CHECK(finalFds     == baselineFds);
    CHECK(finalThreads == baselineThreads);

    // A leak of 64 KB a cycle is 1.3 MB across the twenty cycles and fails.
    CHECK(late <= early + kHeapDriftBytes);
}

// Where the self-check below keeps its blocks. Volatile, so the compiler must
// treat every store as observable: an allocation whose pointer is only ever
// written and freed may otherwise be removed outright in an optimised build,
// and was, in the macOS measurement that set kHeapDriftBytes.
char* volatile gHeldBlocks[20];

TEST_CASE("The leak test's heap reading sees memory that is not freed", "[lifecycle]") {
    // The rule above is only as good as heapInUseBytes(). This holds memory the
    // size of the leak the rule is meant to catch, and requires the reading to
    // see it: a reading that stays at zero (glibc's counters under a
    // sanitizer) fails here, instead of passing the rule above for nothing.
    constexpr size_t kBlock  = 64 * 1024;
    constexpr int    kBlocks = 20;
    const size_t before = heapInUseBytes();
    for (int i = 0; i < kBlocks; ++i) {
        char* p = static_cast<char*>(std::malloc(kBlock));
        REQUIRE(p != nullptr);
        std::memset(p, 0x5a, kBlock);
        gHeldBlocks[i] = p;
    }
    const size_t after = heapInUseBytes();
    for (int i = 0; i < kBlocks; ++i) std::free(gHeldBlocks[i]);
    INFO("heap before=" << before << " after=" << after);
    CHECK(after >= before + kBlocks * kBlock);
}

TEST_CASE("Shutdown without init is safe", "[lifecycle]") {
    // Easy case: shutdown on a never-touched engine. Catches regressions
    // that assume init() has run.
    ClockworkEngine engine;
    engine.shutdown();
    CHECK_FALSE(engine.isRunning());

    engine.shutdown();
    CHECK_FALSE(engine.isRunning());
}

TEST_CASE("Partial-init failure cleans up allocated resources",
          "[lifecycle]") {
    // Drives init() to throw after the scsynth World has been
    // created, worker threads have started, and the audio callback is
    // wired, but before mRunning is set. shutdown()
    // (explicit and via the destructor) must release everything.
    // Headless mode means no AudioDeviceManager / property listener is
    // exercised here; that path needs a non-headless test environment.
    ClockworkEngine::Config cfg;
    cfg.headless = true;
    cfg.udpPort  = 0;

    {
        ClockworkEngine engine;
        engine.init(cfg);
        engine.shutdown();
    }

    const long baselineRss     = readRssKb();
    const int  baselineFds     = countFds();
    const int  baselineThreads = settleThreadCount();

    {
        ClockworkEngine engine;
        engine.testInitFailure = []() { return std::string("injected"); };
        REQUIRE_THROWS_AS(engine.init(cfg), std::runtime_error);
        CHECK_FALSE(engine.isRunning());

        engine.shutdown();
        CHECK_FALSE(engine.isRunning());
    }

    CHECK(countFds()     == baselineFds);
    CHECK(settleThreadCount() == baselineThreads);
    const long rssBudget = baselineRss / 2;
    CHECK(readRssKb() - baselineRss < rssBudget);
}

#endif  // __linux__
