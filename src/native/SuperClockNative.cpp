/*
 * SuperClockNative.cpp — native backend for SuperClock.
 *
 * Owns the SuperClockState struct as a private Impl member. Provides the
 * audio-thread time source (IIR-smoothed wall clock) and the app-thread
 * wallNow() entry point. Shared mutators/getters live in SuperClock.cpp.
 */
#include "SuperClock.h"
#include "native/WallClock.h"
#include "shared_memory.h"

#include <atomic>

struct SuperClock::Impl {
    SuperClockState ownedState;

    // IIR-smoothed audio-thread NTP, written by the audio thread and read
    // by app threads via now(). Stored as bit-pattern in atomic<uint64_t>
    // for a portable atomic double.
    std::atomic<uint64_t> baseNTPBits{0};
    std::atomic<uint64_t> currentAudioThreadNTPBits{0};

    Impl() { SuperClockState::initDefaults(ownedState); }
};

SuperClock::SuperClock() : mImpl(std::make_unique<Impl>()) {}
SuperClock::~SuperClock() = default;

SuperClockState*       SuperClock::state()       { return &mImpl->ownedState; }
const SuperClockState* SuperClock::state() const { return &mImpl->ownedState; }

double SuperClock::now() const {
    const uint64_t bits =
        mImpl->currentAudioThreadNTPBits.load(std::memory_order_acquire);
    if (bits == 0) return wallClockNTP();
    return supersonic::bitsToDouble(bits);
}

double SuperClock::nowAt(double audioCurrentTime) const {
    (void)audioCurrentTime;  // Native time source is IIR; arg unused.
    return now();
}

double SuperClock::wallNow() const {
    return wallClockNTP();
}

double SuperClock::updateAudioThreadNTP(double samplePosition,
                                         double sampleRate,
                                         double audioCurrentTime) {
    (void)audioCurrentTime;

    //   sampleNTP = mBaseNTP + sampleOffsetSec
    //   drift     = wallNow - sampleNTP
    //   mBaseNTP += drift * 0.01   (low-pass converge ~1% per call)
    //   result    = mBaseNTP + sampleOffsetSec
    const double sampleOffsetSec = samplePosition / sampleRate;
    const double wallNow = wallClockNTP();
    const double baseNTP = supersonic::bitsToDouble(
        mImpl->baseNTPBits.load(std::memory_order_relaxed));
    const double drift = wallNow - (baseNTP + sampleOffsetSec);
    const double newBaseNTP = baseNTP + drift * 0.01;
    const double wallNTP = newBaseNTP + sampleOffsetSec;
    mImpl->baseNTPBits.store(supersonic::doubleToBits(newBaseNTP), std::memory_order_relaxed);
    mImpl->currentAudioThreadNTPBits.store(supersonic::doubleToBits(wallNTP),
                                            std::memory_order_release);
    return wallNTP;
}

void SuperClock::resetAudioThreadTime(double samplePosition, double sampleRate) {
    // Re-baseline the IIR: mBaseNTP such that "now at samplePosition" == wallClockNTP().
    const double sampleOffsetSec = samplePosition / sampleRate;
    const double newBaseNTP = wallClockNTP() - sampleOffsetSec;
    mImpl->baseNTPBits.store(supersonic::doubleToBits(newBaseNTP), std::memory_order_relaxed);
    mImpl->currentAudioThreadNTPBits.store(
        supersonic::doubleToBits(newBaseNTP + sampleOffsetSec),
        std::memory_order_release);
}
