/*
 * SuperClockWasm.cpp — WASM backend for SuperClock.
 *
 * Points at a SuperClockState struct in the SAB (its address is provided
 * by audio_processor.cpp via superclock_wasm_init). Provides the
 * audio-thread time source: audio_current_time + ntp_start + drift +
 * global_offset, all read from SAB. Shared mutators/getters live in
 * SuperClock.cpp.
 */
#include "SuperClock.h"
#include "shared_memory.h"

#include <emscripten/emscripten.h>
#include <atomic>

namespace {

SuperClockState*            g_superclock_state = nullptr;
const double*               g_ntp_start_time   = nullptr;
const std::atomic<int32_t>* g_drift_offset     = nullptr;
const std::atomic<int32_t>* g_global_offset    = nullptr;

// Cache published by nowAt(), read by now().
std::atomic<uint64_t> g_last_audio_thread_ntp_bits{0};

}  // namespace

extern "C" {

EMSCRIPTEN_KEEPALIVE
void superclock_wasm_init(SuperClockState* superclock_state,
                          const double* ntp_start_time_ptr,
                          const std::atomic<int32_t>* drift_offset_ptr,
                          const std::atomic<int32_t>* global_offset_ptr) {
    g_superclock_state = superclock_state;
    g_ntp_start_time   = ntp_start_time_ptr;
    g_drift_offset     = drift_offset_ptr;
    g_global_offset    = global_offset_ptr;
}

}  // extern "C"

struct SuperClock::Impl {};

SuperClock::SuperClock() : mImpl(std::make_unique<Impl>()) {}
SuperClock::~SuperClock() = default;

SuperClockState*       SuperClock::state()       { return g_superclock_state; }
const SuperClockState* SuperClock::state() const { return g_superclock_state; }

double SuperClock::now() const {
    return supersonic::bitsToDouble(
        g_last_audio_thread_ntp_bits.load(std::memory_order_acquire));
}

double SuperClock::nowAt(double audioCurrentTime) const {
    const double drift_seconds =
        g_drift_offset ? (g_drift_offset->load(std::memory_order_acquire) / 1000000.0) : 0.0;
    const double ntp_start =
        (g_ntp_start_time && *g_ntp_start_time != 0.0) ? *g_ntp_start_time : 0.0;
    const double global_seconds =
        g_global_offset ? (g_global_offset->load(std::memory_order_relaxed) / 1000.0) : 0.0;
    const double result = audioCurrentTime + ntp_start + drift_seconds + global_seconds;
    g_last_audio_thread_ntp_bits.store(
        supersonic::doubleToBits(result), std::memory_order_release);
    return result;
}

double SuperClock::wallNow() const {
    return 0.0;
}

double SuperClock::updateAudioThreadNTP(double samplePosition,
                                         double sampleRate,
                                         double audioCurrentTime) {
    (void)samplePosition;
    (void)sampleRate;
    return nowAt(audioCurrentTime);
}

void SuperClock::resetAudioThreadTime(double samplePosition, double sampleRate) {
    (void)samplePosition;
    (void)sampleRate;
}
