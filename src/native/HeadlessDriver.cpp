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

// Use the shared implementation from JuceAudioCallback (avoids duplication).
static double wallClockNTP() { return JuceAudioCallback::wallClockNTP(); }

HeadlessDriver::HeadlessDriver()
    : juce::Thread("SuperSonic-Headless") {}

void HeadlessDriver::configure(JuceAudioCallback* callback,
                                SampleLoader* sampleLoader,
                                int sampleRate,
                                int numOutputChannels,
                                int numInputChannels) {
    mCallback          = callback;
    mSampleLoader      = sampleLoader;
    mSampleRate        = (sampleRate > 0) ? sampleRate : 48000;
    mNumOutputChannels = numOutputChannels;
    mNumInputChannels  = numInputChannels;
}

// Shared loop body — called once per block from the platform-specific run() loop.
// Installs pending buffers, derives jitter-free NTP from sample position with
// slow drift correction, calls process_audio, and wakes worker threads.
void HeadlessDriver::processBlock(double& baseNTP, double& samplePos) {
    if (mSampleLoader)
        mSampleLoader->installPendingBuffers();

    double wallNow = wallClockNTP();
    double sampleNTP = baseNTP + samplePos / mSampleRate;
    baseNTP += (wallNow - sampleNTP) * 0.01;
    double ntp = baseNTP + samplePos / mSampleRate;

    process_audio(ntp,
                  static_cast<uint32_t>(mNumOutputChannels),
                  static_cast<uint32_t>(mNumInputChannels));
    samplePos += kBlockSize;

    mCallback->processCount.fetch_add(1, std::memory_order_release);
    mCallback->processCount.notify_all();
}

// Each platform implements run() using its highest-resolution timer.
// Only the sleep mechanism differs; the loop body is shared via processBlock().

#if defined(__linux__)

void HeadlessDriver::run() {
    const int64_t blockNs = 1'000'000'000LL * kBlockSize / mSampleRate;
    struct timespec next;
    clock_gettime(CLOCK_MONOTONIC, &next);
    double baseNTP = wallClockNTP();
    double samplePos = 0.0;

    while (!threadShouldExit()) {
        processBlock(baseNTP, samplePos);
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
    const uint64_t blockNs    = 1'000'000'000ULL * kBlockSize / mSampleRate;
    const uint64_t blockTicks = blockNs * tbi.denom / tbi.numer;
    uint64_t nextWake = mach_absolute_time();
    double baseNTP = wallClockNTP();
    double samplePos = 0.0;

    while (!threadShouldExit()) {
        processBlock(baseNTP, samplePos);
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
    const LONGLONG blockTicks = freq.QuadPart * kBlockSize / mSampleRate;
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    LONGLONG nextWake = now.QuadPart;
    double baseNTP = wallClockNTP();
    double samplePos = 0.0;

    while (!threadShouldExit()) {
        processBlock(baseNTP, samplePos);
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
