#include "RealtimeThread.h"

#if defined(__linux__)
#include <cerrno>
#include <pthread.h>
#include <sched.h>
#endif

namespace supersonic {

#if defined(__linux__)

RealtimeResult elevateCurrentThreadToRealtime() {
    // Capture the current policy first so a denied request can report the
    // unchanged state.
    int currentPolicy = SCHED_OTHER;
    sched_param currentParam{};
    pthread_getschedparam(pthread_self(), &currentPolicy, &currentParam);

    constexpr int kPolicy = SCHED_RR;
    const int lo = sched_get_priority_min(kPolicy);
    const int hi = sched_get_priority_max(kPolicy);

    // A modest realtime priority (low quarter of the RR range): above ordinary
    // SCHED_OTHER work, but below where a system audio server (PipeWire/JACK) or
    // kernel realtime threads sit, so those are not starved.
    const int priority = lo + (hi - lo) / 4;

    sched_param param{};
    param.sched_priority = priority;

    // pthread_setschedparam is atomic: it either applies policy+priority wholly
    // or changes nothing. It returns the error code directly (not via errno).
    const int rc = pthread_setschedparam(pthread_self(), kPolicy, &param);
    if (rc == 0)
        return {RealtimeStatus::Applied, kPolicy, priority, 0};

    const RealtimeStatus status =
        (rc == EPERM) ? RealtimeStatus::NotPermitted : RealtimeStatus::Failed;
    return {status, currentPolicy, currentParam.sched_priority, rc};
}

#else  // non-Linux: the audio thread is left to its existing OS/JUCE behaviour.

RealtimeResult elevateCurrentThreadToRealtime() {
    return {RealtimeStatus::NotSupported, 0, 0, 0};
}

#endif

}  // namespace supersonic
