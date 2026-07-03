#include "RealtimeThread.h"

#if defined(__linux__)
#include <cerrno>
#include <pthread.h>
#include <sched.h>
#elif defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <avrt.h>
#pragma comment(lib, "avrt")
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

#elif defined(_WIN32)

RealtimeResult elevateCurrentThreadToRealtime() {
    // Windows audio threads are NOT protected by default: JUCE's DirectSound
    // backend polls from an ordinary Priority::highest juce::Thread, and even
    // ASIO driver threads aren't necessarily MMCSS-registered. Under CPU load
    // (a compile, a browser) the callback gets preempted for multi-ms bursts
    // → DSP overruns → audible stutter with no callback gap.
    //
    // Two independent measures; either alone is a win:
    //  * MMCSS "Pro Audio" registration — the OS-blessed audio-thread
    //    protection (the same scheduler class the WASAPI engine uses).
    //  * THREAD_PRIORITY_TIME_CRITICAL — the classic boost scsynth has
    //    always applied on Windows.
    DWORD taskIndex = 0;
    HANDLE mmcss = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex);
    if (mmcss)
        AvSetMmThreadPriority(mmcss, AVRT_PRIORITY_CRITICAL);
    const BOOL boosted =
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    const int resulting = GetThreadPriority(GetCurrentThread());

    // `policy` reports MMCSS engagement (1 = registered); `priority` the
    // resulting Win32 thread priority.
    if (mmcss || boosted)
        return {RealtimeStatus::Applied, mmcss ? 1 : 0, resulting, 0};
    return {RealtimeStatus::Failed, 0, resulting,
            static_cast<int>(GetLastError())};
}

#else  // other platforms: the audio thread keeps its existing OS/JUCE behaviour.

RealtimeResult elevateCurrentThreadToRealtime() {
    return {RealtimeStatus::NotSupported, 0, 0, 0};
}

#endif

}  // namespace supersonic
