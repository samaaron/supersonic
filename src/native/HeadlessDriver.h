/*
 * HeadlessDriver.h — Timer-driven audio processing without a real audio device.
 *
 * When no audio hardware is available (headless mode), this thread calls
 * process_audio() at the correct block rate (matches whatever block size
 * the engine configured, typically 128 samples), using platform-specific
 * high-resolution timers for accurate timing:
 *
 *   Linux:   clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME)
 *   macOS:   mach_wait_until()
 *   Windows: WaitableTimer with CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
 *
 * Worker threads (ReplyReader, DebugReader) are woken after each block,
 * so OSC replies flow exactly as they would with a real audio device.
 */
#pragma once

#include <juce_core/juce_core.h>
#include <cstdint>

class JuceAudioCallback;
class SampleLoader;
class SuperClock;

class HeadlessDriver : public juce::Thread {
public:
    HeadlessDriver();

    void configure(JuceAudioCallback* callback,
                   SampleLoader* sampleLoader,
                   int sampleRate,
                   int blockSize,
                   int numOutputChannels,
                   int numInputChannels);

    // Must be set before run() is called.
    void setSuperClock(SuperClock* sc) { mSuperClock = sc; }

    void run() override;

    // Bound on how far the timer loop replays missed blocks after a scheduling
    // gap (thread starvation, sleep/wake). Beyond this the loop re-anchors to
    // the current time and drops the backlog rather than firing a back-to-back
    // "catch-up" burst of process_audio() calls. ~8 blocks (~21 ms at 128/48k)
    // is far beyond normal timer jitter (<1 block) yet caps a stampede hard.
    static constexpr int kMaxCatchupBlocks = 8;

    // Given the just-advanced wake deadline, the current time, and the block
    // period — all in the SAME tick unit (ns, mach ticks, or QPC ticks) —
    // return the deadline to actually sleep until. If the loop has fallen more
    // than kMaxCatchupBlocks behind `now`, re-anchor to `now` (drop backlog);
    // otherwise keep the scheduled deadline so small lags realign smoothly.
    // Pure and unit-agnostic so the anti-stampede rule is unit-testable without
    // real timing. Sample position is unaffected — only the wall-clock deadline
    // moves, so audio stays sample-continuous; it just stops sprinting.
    static int64_t cappedNextWake(int64_t nextWake, int64_t now,
                                  int64_t blockTicks);

private:
    // Shared loop body: install buffers, derive NTP via SuperClock,
    // process audio, wake workers. Called once per block.
    void processBlock(double& samplePos);

    // Ticks the engine at this block size. Set explicitly from
    // SupersonicEngine::Config so the tick rate is deterministic and
    // independent of whatever buffer size a transient real device may
    // have left on the audio callback (e.g. failed-init fallback path).
    int mBlockSize = 128;

    JuceAudioCallback* mCallback         = nullptr;
    SampleLoader*      mSampleLoader     = nullptr;
    SuperClock*        mSuperClock       = nullptr;
    int                mSampleRate        = 48000;
    int                mNumOutputChannels = 2;
    int                mNumInputChannels  = 0;
};
