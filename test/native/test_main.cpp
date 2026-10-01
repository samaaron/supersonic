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
#include "DebugTail.h"
#include <catch2/catch_session.hpp>
#include <catch2/catch_test_case_info.hpp>
#include <catch2/reporters/catch_reporter_event_listener.hpp>
#include <catch2/reporters/catch_reporter_registrars.hpp>
#include <juce_events/juce_events.h>
#include <cstdio>
#include <string>
#include <vector>
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#endif

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

// ─── The arguments as ctest hands them ────────────────────────────────────────
// ctest runs each case on its own, giving the binary the case's name on the
// command line (catch_discover_tests). Two things go wrong with that on
// Windows, and the fix for both is here, once, rather than in every name:
//
//   - a narrow main() gets argv through the console's legacy code page, so a
//     name with a dash or an arrow in it arrives mangled and matches no case.
//     The command line is read again as Unicode and converted to UTF-8, which
//     is what the names in the binary are;
//   - Catch2's parser takes a token beginning with '/' as a switch, Windows
//     style, so a case named for a verb ("/clockwork/notify is …") is an
//     "Unrecognised token" and nothing runs. A test spec may escape any
//     character with a backslash, so a leading '/' is given one. Done on
//     every platform, so the escaping is exercised where it is not needed too.
namespace {

std::vector<std::string> argumentsAsUtf8(int argc, char* argv[]) {
    std::vector<std::string> args;
#if defined(_WIN32)
    int wargc = 0;
    if (LPWSTR* wargv = CommandLineToArgvW(GetCommandLineW(), &wargc)) {
        for (int i = 0; i < wargc; ++i) {
            const int n = WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, nullptr, 0, nullptr, nullptr);
            std::string s(n > 0 ? n - 1 : 0, '\0');
            if (n > 1) WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, &s[0], n, nullptr, nullptr);
            args.push_back(std::move(s));
        }
        LocalFree(wargv);
    }
    if (args.empty())
#endif
    for (int i = 0; i < argc; ++i) args.emplace_back(argv[i]);
    for (size_t i = 1; i < args.size(); ++i)
        if (!args[i].empty() && args[i][0] == '/') args[i].insert(0, 1, '\\');
    return args;
}

int runSession(int argc, char* argv[]) {
    std::vector<std::string> args = argumentsAsUtf8(argc, argv);
    std::vector<const char*> ptrs;
    for (const std::string& a : args) ptrs.push_back(a.c_str());
    return Catch::Session().run(static_cast<int>(ptrs.size()), ptrs.data());
}

} // namespace

int main(int argc, char* argv[]) {
    // A fatal signal prints the last engine debug lines and a backtrace
    // (DebugTail.h); Catch2 reports first, then hands the signal back here.
    debug_tail::installFatalHandlers();
    juce::ScopedJuceInitialiser_GUI juceInit;
    return runSession(argc, argv);
}
