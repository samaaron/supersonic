/*
 * test_lifecycle_leaks.cpp - Detect resource leaks across start/stop cycles.
 *
 * Drives 20 init/shutdown cycles in headless mode and asserts that file
 * descriptors and thread count return to baseline. RSS is permitted mild
 * drift but capped well below what an obvious leak would produce.
 *
 * Scope: catches FD/thread regressions in the graceful no-throw lifecycle.
 * Does NOT exercise partial-init failure (init() throwing partway),
 * which needs a separate test with a failure-injection hook on the engine.
 * Does NOT install the macOS CoreAudio property listener, since the test
 * runs in headless mode. The RSS budget is generous, so small slow leaks
 * may not register.
 *
 * Linux-only: reads /proc/self. Test cases are compiled out on other
 * platforms.
 */
#include "SupersonicEngine.h"
#include <catch2/catch_test_macros.hpp>

#if defined(__linux__)

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <string>

namespace {

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

}  // namespace

TEST_CASE("Repeated init/shutdown does not leak FDs or threads", "[lifecycle][stress]") {
    constexpr int kCycles = 20;

    SupersonicEngine::Config cfg;
    cfg.headless = true;
    cfg.udpPort  = 0;

    // Warm-up cycle: first boot allocates one-shot caches (JUCE's device-type
    // list, sndfile lookup tables, etc.) that persist until process exit and
    // would otherwise be misread as a leak. Measure baseline AFTER warm-up.
    {
        SupersonicEngine engine;
        engine.init(cfg);
        REQUIRE(engine.isRunning());
        engine.shutdown();
    }

    const long baselineRss      = readRssKb();
    const int  baselineFds      = countFds();
    const int  baselineThreads  = countThreads();

    REQUIRE(baselineRss > 0);
    REQUIRE(baselineFds > 0);
    REQUIRE(baselineThreads > 0);

    INFO("baseline rss=" << baselineRss << "kb fds=" << baselineFds
                          << " threads=" << baselineThreads);

    for (int i = 0; i < kCycles; ++i) {
        SupersonicEngine engine;
        engine.init(cfg);
        REQUIRE(engine.isRunning());
        engine.shutdown();
        REQUIRE_FALSE(engine.isRunning());
    }

    const long finalRss     = readRssKb();
    const int  finalFds     = countFds();
    const int  finalThreads = countThreads();

    INFO("after " << kCycles << " cycles: rss=" << finalRss << "kb fds="
                  << finalFds << " threads=" << finalThreads);

    // FDs and threads must return to baseline exactly.
    CHECK(finalFds     == baselineFds);
    CHECK(finalThreads == baselineThreads);

    // RSS may drift from heap fragmentation; 50% of baseline is a generous
    // ceiling that a real leak would blow past within a few cycles.
    const long rssBudget = baselineRss / 2;
    CHECK(finalRss - baselineRss < rssBudget);
}

TEST_CASE("Shutdown without init is safe", "[lifecycle]") {
    // Easy case: shutdown on a never-touched engine. Catches regressions
    // that assume init() has run.
    SupersonicEngine engine;
    engine.shutdown();
    CHECK_FALSE(engine.isRunning());

    engine.shutdown();
    CHECK_FALSE(engine.isRunning());
}

TEST_CASE("Partial-init failure cleans up allocated resources",
          "[lifecycle]") {
    // Drives init() to throw after the scsynth World has been
    // created, worker threads have started, and the audio callback is
    // wired to the SampleLoader, but before mRunning is set. shutdown()
    // (explicit and via the destructor) must release everything.
    // Headless mode means no AudioDeviceManager / property listener is
    // exercised here; that path needs a non-headless test environment.
    SupersonicEngine::Config cfg;
    cfg.headless = true;
    cfg.udpPort  = 0;

    {
        SupersonicEngine engine;
        engine.init(cfg);
        engine.shutdown();
    }

    const long baselineRss     = readRssKb();
    const int  baselineFds     = countFds();
    const int  baselineThreads = countThreads();

    {
        SupersonicEngine engine;
        engine.testInitFailure = []() { return std::string("injected"); };
        REQUIRE_THROWS_AS(engine.init(cfg), std::runtime_error);
        CHECK_FALSE(engine.isRunning());

        engine.shutdown();
        CHECK_FALSE(engine.isRunning());
    }

    CHECK(countFds()     == baselineFds);
    CHECK(countThreads() == baselineThreads);
    const long rssBudget = baselineRss / 2;
    CHECK(readRssKb() - baselineRss < rssBudget);
}

#endif  // __linux__
