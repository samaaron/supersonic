/*
 * HeadlessDriver.cpp — Platform-specific high-resolution timer implementations.
 */
#include "HeadlessDriver.h"
#include "JuceAudioCallback.h"
#include "SampleLoader.h"
#include "SuperClock.h"
#include "lanes/lanes.h"

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

    // Sample clock: headless has no DAC, so "audible" == render time
    // (latency 0). Keeps scope streams and their tests time-anchored.
    mSuperClock->publishSampleClock(samplePos, static_cast<double>(mSampleRate),
                                    ntp, 0);

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

// Re-anchor the wake deadline when the loop has fallen more than
// kMaxCatchupBlocks behind, so a scheduling gap can't turn into a back-to-back
// catch-up burst. Unit-agnostic (see header); the caller passes now, deadline,
// and block period in one consistent tick unit.
int64_t HeadlessDriver::cappedNextWake(int64_t nextWake, int64_t now,
                                       int64_t blockTicks) {
    const int64_t maxLag = blockTicks * kMaxCatchupBlocks;
    if (now - nextWake > maxLag)
        return now;  // drop the backlog rather than replaying every missed block
    return nextWake;
}

// Each platform implements run() using its highest-resolution timer.
// Only the sleep mechanism differs; the loop body is shared via processBlock(),
// and each loop caps catch-up via cappedNextWake() after advancing the deadline.

#if defined(__linux__)

static int64_t timespecToNs(const struct timespec& ts) {
    return static_cast<int64_t>(ts.tv_sec) * 1'000'000'000LL + ts.tv_nsec;
}
static struct timespec nsToTimespec(int64_t ns) {
    struct timespec ts;
    ts.tv_sec  = static_cast<time_t>(ns / 1'000'000'000LL);
    ts.tv_nsec = static_cast<long>(ns % 1'000'000'000LL);
    return ts;
}

void HeadlessDriver::run() {
    const int64_t blockNs = 1'000'000'000LL * mBlockSize / mSampleRate;
    struct timespec next;
    clock_gettime(CLOCK_MONOTONIC, &next);
    double samplePos = 0.0;
    mSuperClock->resetAudioThreadTime(samplePos, mSampleRate);

    while (!threadShouldExit()) {
        processBlock(samplePos);
        struct timespec nowTs;
        clock_gettime(CLOCK_MONOTONIC, &nowTs);
        next = nsToTimespec(cappedNextWake(timespecToNs(next) + blockNs,
                                           timespecToNs(nowTs), blockNs));
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
        nextWake = static_cast<uint64_t>(cappedNextWake(
            static_cast<int64_t>(nextWake + blockTicks),
            static_cast<int64_t>(mach_absolute_time()),
            static_cast<int64_t>(blockTicks)));
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
        QueryPerformanceCounter(&now);
        nextWake = cappedNextWake(nextWake + blockTicks, now.QuadPart, blockTicks);
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
