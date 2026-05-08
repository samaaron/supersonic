/*
 * test_main.cpp — Custom Catch2 main with JUCE initialisation.
 *
 * JUCE requires COM init on Windows (ScopedJuceInitialiser_GUI)
 * before any AudioDeviceManager or Thread operations.
 *
 * Also installs a global RT-alloc listener: process_audio() sets a
 * thread-local guard; the test binary's operator new/delete overrides
 * (in test_rt_alloc.cpp) count allocations under the guard. This
 * listener resets the counter at the start of every test and warns at
 * the end if any audio-thread allocation fired. Warning rather than
 * failing — catalogues offenders without breaking the suite.
 */
#include "rt_alloc.h"
#include <catch2/catch_session.hpp>
#include <catch2/catch_test_case_info.hpp>
#include <catch2/reporters/catch_reporter_event_listener.hpp>
#include <catch2/reporters/catch_reporter_registrars.hpp>
#include <juce_events/juce_events.h>
#include <cstdio>
#include <string>

namespace {

struct RTAllocListener : Catch::EventListenerBase {
    using Catch::EventListenerBase::EventListenerBase;

    void testCaseStarting(Catch::TestCaseInfo const&) override {
        rt_alloc::reset();
    }

    void testCaseEnded(Catch::TestCaseStats const& stats) override {
        // The [rt_alloc] tests manage their own counts and assert on them
        // explicitly; passive listening would double-count.
        if (stats.testInfo->tagsAsString().find("[rt_alloc]") != std::string::npos) {
            return;
        }
        int64_t a = rt_alloc::g_allocs.load(std::memory_order_relaxed);
        int64_t f = rt_alloc::g_frees.load(std::memory_order_relaxed);
        if (a > 0 || f > 0) {
            std::fprintf(stderr,
                "\n[rt-alloc] %s: %lld allocs / %lld frees on audio thread\n",
                stats.testInfo->name.c_str(), (long long)a, (long long)f);
        }
    }
};

} // namespace

CATCH_REGISTER_LISTENER(RTAllocListener)

int main(int argc, char* argv[]) {
    juce::ScopedJuceInitialiser_GUI juceInit;
    return Catch::Session().run(argc, argv);
}
