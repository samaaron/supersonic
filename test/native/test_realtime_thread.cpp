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

#else  // non-Linux

TEST_CASE("elevateCurrentThreadToRealtime is a no-op off Linux", "[realtime]") {
    // The helper does nothing off Linux.
    REQUIRE(supersonic::elevateCurrentThreadToRealtime().status
            == supersonic::RealtimeStatus::NotSupported);
}

#endif
