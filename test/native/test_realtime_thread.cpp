/*
 * test_realtime_thread.cpp — the audio-thread realtime-promotion helper
 * (elevateCurrentThreadToRealtime, src/native/RealtimeThread.*). See
 * sonic-pi#3543.
 *
 * The tests pin two properties: when rtprio is permitted the calling thread
 * becomes SCHED_RR, and when it is denied the thread is left exactly as it was
 * (no abort, no change).
 */
#include <catch2/catch_test_macros.hpp>

#include <thread>

#include "src/native/RealtimeThread.h"

#if defined(__linux__)
#include <pthread.h>
#include <sched.h>

using supersonic::elevateCurrentThreadToRealtime;
using supersonic::RealtimeStatus;

TEST_CASE("elevateCurrentThreadToRealtime: graceful fallback or genuine RT", "[realtime]") {
    // Run on a dedicated thread: the helper permanently changes the calling
    // thread's scheduling, so promoting the shared Catch2 runner would leak
    // SCHED_RR into later tests. Capture as plain values, assert on this thread.
    RealtimeStatus status{};
    int beforePolicy = 0, afterPolicy = 0, beforePrio = 0, afterPrio = 0;
    bool readOk = false;

    std::thread([&] {
        sched_param before{}, after{};
        const bool gotBefore = pthread_getschedparam(pthread_self(), &beforePolicy, &before) == 0;
        beforePrio = before.sched_priority;

        status = elevateCurrentThreadToRealtime().status;

        const bool gotAfter = pthread_getschedparam(pthread_self(), &afterPolicy, &after) == 0;
        afterPrio = after.sched_priority;
        readOk = gotBefore && gotAfter;
    }).join();

    REQUIRE(readOk);
    if (status == RealtimeStatus::Applied) {
        // Permitted: the thread is now realtime.
        REQUIRE(afterPolicy == SCHED_RR);
        REQUIRE(afterPrio >= sched_get_priority_min(SCHED_RR));
        REQUIRE(afterPrio <= sched_get_priority_max(SCHED_RR));
    } else {
        // Denied: the thread must be left untouched.
        REQUIRE((status == RealtimeStatus::NotPermitted || status == RealtimeStatus::Failed));
        REQUIRE(afterPolicy == beforePolicy);
        REQUIRE(afterPrio == beforePrio);
    }
}

TEST_CASE("elevateCurrentThreadToRealtime: idempotent and never throws", "[realtime]") {
    // Calling twice (as a cold-swap re-opening the device would) must be safe
    // and converge to the same status. On a dedicated thread for the same
    // runner-isolation reason as above.
    RealtimeStatus first{}, second{};
    std::thread([&] {
        first  = elevateCurrentThreadToRealtime().status;
        second = elevateCurrentThreadToRealtime().status;
    }).join();
    REQUIRE(first == second);
}

#elif defined(_WIN32)

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

TEST_CASE("elevateCurrentThreadToRealtime: Windows gets MMCSS + "
          "time-critical, never a silent no-op", "[realtime]") {
    // Windows must actively protect the audio thread: JUCE's DirectSound
    // backend polls from an ordinary-priority thread with no MMCSS
    // registration, so without elevation any CPU load preempts the render
    // into audible overruns. On a dedicated thread (the promotion is
    // permanent) with plain-value capture; assertions on the runner thread.
    supersonic::RealtimeResult result{};
    int win32Priority = 0;
    std::thread([&] {
        result        = supersonic::elevateCurrentThreadToRealtime();
        win32Priority = GetThreadPriority(GetCurrentThread());
    }).join();

    REQUIRE(result.status == supersonic::RealtimeStatus::Applied);
    REQUIRE(win32Priority == THREAD_PRIORITY_TIME_CRITICAL);
    // policy carries MMCSS engagement (1 = "Pro Audio" registered). Stripped
    // environments can lack the MMCSS service, and TIME_CRITICAL alone still
    // counts as Applied — assert the field is well-formed, not its value.
    REQUIRE((result.policy == 0 || result.policy == 1));
    REQUIRE(result.error == 0);
}

TEST_CASE("elevateCurrentThreadToRealtime: idempotent on Windows", "[realtime]") {
    supersonic::RealtimeStatus first{}, second{};
    std::thread([&] {
        first  = supersonic::elevateCurrentThreadToRealtime().status;
        second = supersonic::elevateCurrentThreadToRealtime().status;
    }).join();
    REQUIRE(first == supersonic::RealtimeStatus::Applied);
    REQUIRE(first == second);
}

#else  // other platforms (macOS): the helper stays a documented no-op.

TEST_CASE("elevateCurrentThreadToRealtime is a no-op on this platform", "[realtime]") {
    REQUIRE(supersonic::elevateCurrentThreadToRealtime().status
            == supersonic::RealtimeStatus::NotSupported);
}

#endif
