/*
 * HeadlessDriver.cpp — Platform-specific high-resolution timer implementations.
 */
#include "HeadlessDriver.h"
#include "JuceAudioCallback.h"
#include "SampleLoader.h"

#if defined(__linux__)
  #include <time.h>
#elif defined(__APPLE__)
  #include <mach/mach_time.h>
#elif defined(_WIN32)
  #include <windows.h>
#endif

static constexpr double NTP_EPOCH_OFFSET = 2208988800.0;

HeadlessDriver::HeadlessDriver()
    : juce::Thread("SuperSonic-Headless") {}

void HeadlessDriver::configure(JuceAudioCallback* callback,
                                SampleLoader* sampleLoader,
                                int sampleRate,
                                int numOutputChannels,
                                int numInputChannels) {
    mCallback          = callback;
    mSampleLoader      = sampleLoader;
    mSampleRate        = sampleRate;
    mNumOutputChannels = numOutputChannels;
    mNumInputChannels  = numInputChannels;
}

// Each platform implements run() using its highest-resolution timer.
// The loop body is identical: install pending buffers, compute wall-clock
// NTP time, call process_audio(), wake worker threads, then sleep until
// the next block boundary.

#if defined(__linux__)

void HeadlessDriver::run() {
    const int64_t blockNs = 1'000'000'000LL * kBlockSize / mSampleRate;

    struct timespec next;
    clock_gettime(CLOCK_MONOTONIC, &next);

    while (!threadShouldExit()) {
        if (mSampleLoader)
            mSampleLoader->installPendingBuffers();

        double wallNTP = static_cast<double>(juce::Time::currentTimeMillis()) * 0.001
                         + NTP_EPOCH_OFFSET;

        process_audio(wallNTP,
                      static_cast<uint32_t>(mNumOutputChannels),
                      static_cast<uint32_t>(mNumInputChannels));

        mCallback->processCount.fetch_add(1, std::memory_order_release);
        mCallback->processCount.notify_all();

        // Advance absolute deadline by one block
        next.tv_nsec += blockNs;
        while (next.tv_nsec >= 1'000'000'000L) {
            next.tv_nsec -= 1'000'000'000L;
            next.tv_sec++;
        }
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, nullptr);
    }
}

#elif defined(__APPLE__)

void HeadlessDriver::run() {
    mach_timebase_info_data_t tbi;
    mach_timebase_info(&tbi);

    // Block duration in mach absolute time ticks
    const uint64_t blockNs    = 1'000'000'000ULL * kBlockSize / mSampleRate;
    const uint64_t blockTicks = blockNs * tbi.denom / tbi.numer;

    uint64_t nextWake = mach_absolute_time();

    while (!threadShouldExit()) {
        if (mSampleLoader)
            mSampleLoader->installPendingBuffers();

        double wallNTP = static_cast<double>(juce::Time::currentTimeMillis()) * 0.001
                         + NTP_EPOCH_OFFSET;

        process_audio(wallNTP,
                      static_cast<uint32_t>(mNumOutputChannels),
                      static_cast<uint32_t>(mNumInputChannels));

        mCallback->processCount.fetch_add(1, std::memory_order_release);
        mCallback->processCount.notify_all();

        nextWake += blockTicks;
        mach_wait_until(nextWake);
    }
}

#elif defined(_WIN32)

void HeadlessDriver::run() {
    // CREATE_WAITABLE_TIMER_HIGH_RESOLUTION: sub-millisecond resolution
    // Available since Windows 10 1803. Falls back to standard timer on older builds.
    HANDLE timer = CreateWaitableTimerExW(
        nullptr, nullptr,
        CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);

    if (!timer)
        timer = CreateWaitableTimerW(nullptr, FALSE, nullptr);

    // Block duration in 100ns units (negative = relative interval)
    const LONGLONG blockHns = 10'000'000LL * kBlockSize / mSampleRate;

    while (!threadShouldExit()) {
        if (mSampleLoader)
            mSampleLoader->installPendingBuffers();

        double wallNTP = static_cast<double>(juce::Time::currentTimeMillis()) * 0.001
                         + NTP_EPOCH_OFFSET;

        process_audio(wallNTP,
                      static_cast<uint32_t>(mNumOutputChannels),
                      static_cast<uint32_t>(mNumInputChannels));

        mCallback->processCount.fetch_add(1, std::memory_order_release);
        mCallback->processCount.notify_all();

        LARGE_INTEGER due;
        due.QuadPart = -blockHns;
        SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE);
        WaitForSingleObject(timer, INFINITE);
    }

    if (timer)
        CloseHandle(timer);
}

#endif
