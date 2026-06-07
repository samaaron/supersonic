/*
 * HeadlessDriver.cpp — Platform-specific high-resolution timer implementations.
 */
#include "HeadlessDriver.h"
#include "JuceAudioCallback.h"
#include "SampleLoader.h"
#include "SuperClock.h"

#include <algorithm>
#include <cstdint>

#if defined(__linux__)
  #include <time.h>
#elif defined(__APPLE__)
  #include <mach/mach_time.h>
#elif defined(_WIN32)
  #include <windows.h>
#endif

// wallClockNTP() comes from WallClock.h (included via JuceAudioCallback.h)

HeadlessDriver::HeadlessDriver()
    : juce::Thread("SuperSonic-Headless") {}

void HeadlessDriver::configure(JuceAudioCallback* callback,
                                SampleLoader* sampleLoader,
                                int sampleRate,
                                int blockSize,
                                int numOutputChannels,
                                int numInputChannels) {
    mCallback          = callback;
    mSampleLoader      = sampleLoader;
    mSampleRate        = (sampleRate > 0) ? sampleRate : 48000;
    mBlockSize         = (blockSize  > 0) ? blockSize  : 128;
    mNumOutputChannels = numOutputChannels;
    mNumInputChannels  = numInputChannels;
}

// Shared per-block loop body — installs pending buffers, derives NTP via
// SuperClock, drains Link Audio inputs into private buses, calls
// process_audio, publishes Link Audio outputs/aux sinks, wakes workers.
void HeadlessDriver::processBlock(double& samplePos) {
    if (mSampleLoader)
        mSampleLoader->installPendingBuffers();

    const double ntp = mSuperClock->updateAudioThreadNTP(samplePos, mSampleRate);

    // One Link-clock capture per block, passed to both drain and
    // publish. Two captures with process_audio between would let the
    // published stream's beat-time advance at process_audio cost
    // rather than sample rate.
    const uint64_t hostMicros =
        static_cast<uint64_t>(std::max<int64_t>(0, mSuperClock->linkClockMicros()));

    renderAudioBlock(*mSuperClock,
                     static_cast<uint32_t>(mBlockSize),
                     static_cast<uint32_t>(mNumOutputChannels),
                     static_cast<uint32_t>(mNumInputChannels),
                     static_cast<uint32_t>(mSampleRate),
                     ntp, hostMicros);
    samplePos += mBlockSize;

    mCallback->processCount.fetch_add(1, std::memory_order_release);
    mCallback->processCount.notify_all();
}

// Each platform implements run() using its highest-resolution timer.
// Only the sleep mechanism differs; the loop body is shared via processBlock().

#if defined(__linux__)

void HeadlessDriver::run() {
    const int64_t blockNs = 1'000'000'000LL * mBlockSize / mSampleRate;
    struct timespec next;
    clock_gettime(CLOCK_MONOTONIC, &next);
    double samplePos = 0.0;
    mSuperClock->resetAudioThreadTime(samplePos, mSampleRate);

    while (!threadShouldExit()) {
        processBlock(samplePos);
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
    const uint64_t blockNs    = 1'000'000'000ULL * mBlockSize / mSampleRate;
    const uint64_t blockTicks = blockNs * tbi.denom / tbi.numer;
    uint64_t nextWake = mach_absolute_time();
    double samplePos = 0.0;
    mSuperClock->resetAudioThreadTime(samplePos, mSampleRate);

    while (!threadShouldExit()) {
        processBlock(samplePos);
        nextWake += blockTicks;
        mach_wait_until(nextWake);
    }
}

#elif defined(_WIN32)

void HeadlessDriver::run() {
    HANDLE timer = CreateWaitableTimerExW(
        nullptr, nullptr,
        CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (!timer)
        timer = CreateWaitableTimerW(nullptr, FALSE, nullptr);

    LARGE_INTEGER freq;
    QueryPerformanceFrequency(&freq);
    const LONGLONG blockTicks = freq.QuadPart * mBlockSize / mSampleRate;
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    LONGLONG nextWake = now.QuadPart;
    double samplePos = 0.0;
    mSuperClock->resetAudioThreadTime(samplePos, mSampleRate);

    while (!threadShouldExit()) {
        processBlock(samplePos);
        nextWake += blockTicks;
        QueryPerformanceCounter(&now);
        LONGLONG remaining = nextWake - now.QuadPart;
        if (remaining > 0) {
            LARGE_INTEGER due;
            due.QuadPart = -(remaining * 10'000'000LL / freq.QuadPart);
            SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE);
            WaitForSingleObject(timer, INFINITE);
        }
    }

    if (timer)
        CloseHandle(timer);
}

#endif
