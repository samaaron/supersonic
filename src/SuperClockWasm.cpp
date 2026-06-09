/*
 * SuperClockWasm.cpp — WASM backend for SuperClock.
 *
 * Points at a SuperClockState struct in the SAB (its address is provided
 * by audio_processor.cpp via superclock_wasm_init). All Link Audio APIs
 * are stubs — the browser can't host Link's asio thread or do UDP
 * multicast, so the same header surface returns sensible defaults here.
 */
#include "SuperClock.h"
#include "shared_memory.h"

#include <emscripten/emscripten.h>
#include <atomic>
#include <cstring>

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

// WASM binds its SAB region via superclock_wasm_init; this is the native seam.
void SuperClock::bindStateToShm(SuperClockState*) {}

using supersonic::doubleToBits;
using supersonic::bitsToDouble;

// ─── Mutators ───────────────────────────────────────────────────────────

void SuperClock::setBpm(double bpm, double atNtpSeconds) {
    (void)atNtpSeconds;
    // Clamp to a positive minimum — beat math divides by bpm and would
    // otherwise write ±Infinity / NaN into beat_origin_ntp.
    if (!(bpm >= 1.0)) bpm = 1.0;  // also rejects NaN
    SuperClockState* s = state();
    if (!s) return;
    s->bpm.store(doubleToBits(bpm), std::memory_order_relaxed);
}

void SuperClock::setIsPlaying(bool playing, double atNtpSeconds) {
    SuperClockState* s = state();
    if (!s) return;
    s->is_playing_at_ntp.store(doubleToBits(atNtpSeconds), std::memory_order_relaxed);
    s->is_playing.store(playing ? 1u : 0u, std::memory_order_relaxed);
}

void SuperClock::setLinkEnabled(bool enabled) {
    SuperClockState* s = state();
    if (!s) return;
    if (enabled) s->flags.fetch_or(SC_FLAG_LINK_ENABLED,  std::memory_order_relaxed);
    else         s->flags.fetch_and(~SC_FLAG_LINK_ENABLED, std::memory_order_relaxed);
}

void SuperClock::setStartStopSyncEnabled(bool enabled) {
    SuperClockState* s = state();
    if (!s) return;
    if (enabled) s->flags.fetch_or(SC_FLAG_START_STOP_SYNC,  std::memory_order_relaxed);
    else         s->flags.fetch_and(~SC_FLAG_START_STOP_SYNC, std::memory_order_relaxed);
}

void SuperClock::requestBeatAtTime(double beat, double atNtpSeconds, double quantum) {
    (void)quantum;
    SuperClockState* s = state();
    if (!s) return;
    const double bpm = bitsToDouble(s->bpm.load(std::memory_order_relaxed));
    const double newOrigin = atNtpSeconds - beat * 60.0 / bpm;
    s->beat_origin_ntp.store(doubleToBits(newOrigin), std::memory_order_relaxed);
}

void SuperClock::forceBeatAtTime(double beat, double atNtpSeconds, double quantum) {
    requestBeatAtTime(beat, atNtpSeconds, quantum);
}

// ─── Getters ────────────────────────────────────────────────────────────

void* SuperClock::audioThreadLinkAudioPtr() { return nullptr; }

void SuperClock::requestSetLinkEnabledAsync(bool enabled) {
    setLinkEnabled(enabled);
}

bool SuperClock::isLinkEnabled() const {
    const SuperClockState* s = state();
    if (!s) return false;
    return (s->flags.load(std::memory_order_relaxed) & SC_FLAG_LINK_ENABLED) != 0u;
}

bool SuperClock::isStartStopSyncEnabled() const {
    const SuperClockState* s = state();
    if (!s) return false;
    return (s->flags.load(std::memory_order_relaxed) & SC_FLAG_START_STOP_SYNC) != 0u;
}

size_t SuperClock::numPeers() const {
    return 0;
}

// ─── Link-clock-domain RPC (stubs — no Link in WASM) ───────────────────

int64_t SuperClock::linkClockMicros() const  { return 0; }
int64_t SuperClock::timeForIsPlayingMicros() const { return 0; }
double  SuperClock::beatAtLinkTime(int64_t, double) const { return 0.0; }
double  SuperClock::phaseAtLinkTime(int64_t, double) const { return 0.0; }
int64_t SuperClock::timeAtBeatLinkMicros(double, double) const { return 0; }

double  SuperClock::linkMicrosToNtpSeconds(int64_t m) const {
    return static_cast<double>(m) / 1'000'000.0;
}
int64_t SuperClock::ntpSecondsToLinkMicros(double s) const {
    return static_cast<int64_t>(s * 1'000'000.0);
}

// ─── Link callbacks / Link Audio (stubs) ─────────────────────────────────

void SuperClock::setTempoChangedCallback(std::function<void(double)>) {}
void SuperClock::setNumPeersChangedCallback(std::function<void(std::size_t)>) {}
void SuperClock::setStartStopChangedCallback(std::function<void(bool, int64_t)>) {}

// ─── MIDI follower timelines (no MIDI subsystem in WASM) ─────────────────
// Only the Link timeline (id 0) exists; everything else is a 60-BPM stub.
int  SuperClock::claimMidiTimeline(const char*, const char*) { return -1; }
void SuperClock::freeMidiTimeline(int)                {}
int  SuperClock::resolveTimeline(const char* name) const {
    return (!name || !*name || std::strcmp(name, "link") == 0) ? 0 : -1;
}
int  SuperClock::resolveOrClaimTimeline(const char* name) { return resolveTimeline(name); }
void SuperClock::midiTimelinePulse(int, uint64_t) {}
void SuperClock::setMidiTimelineTempo(int, double)         {}
void SuperClock::setMidiTimelineTransport(int, int, double) {}
void SuperClock::tickMidiStaleness()                       {}

double SuperClock::timelineBpm(int id) const { return id == 0 ? getBpm() : 60.0; }
bool   SuperClock::timelineIsPlaying(int id) const { return id == 0 ? isPlaying() : false; }
int64_t SuperClock::timelineTimeForIsPlayingMicros(int id) const {
    return id == 0 ? timeForIsPlayingMicros() : 0;
}
double SuperClock::timelineBeatAtLinkTime(int id, int64_t t, double q) const {
    return id == 0 ? beatAtLinkTime(t, q) : 0.0;
}
double SuperClock::timelinePhaseAtLinkTime(int id, int64_t t, double q) const {
    return id == 0 ? phaseAtLinkTime(t, q) : 0.0;
}
int64_t SuperClock::timelineTimeAtBeatLinkMicros(int id, double b, double q) const {
    return id == 0 ? timeAtBeatLinkMicros(b, q) : 0;
}
std::vector<SuperClock::TimelineInfo> SuperClock::listTimelines() const {
    TimelineInfo link;
    link.name = "link";
    link.raw = "link";
    link.bpm = getBpm();
    link.clocking = true;
    return { link };
}
void SuperClock::setTimelinesChangedCallback(std::function<void()>) {}

void SuperClock::setLinkVisibility(LinkVisibility)            {}
SuperClock::LinkVisibility SuperClock::getLinkVisibility() const {
    return LinkVisibility::Off;
}
void SuperClock::setLinkAudioPublish(bool publish) {
    SuperClockState* s = state();
    if (!s) return;
    if (publish) s->flags.fetch_or(SC_FLAG_LINK_AUDIO_PUBLISH,  std::memory_order_relaxed);
    else         s->flags.fetch_and(~SC_FLAG_LINK_AUDIO_PUBLISH, std::memory_order_relaxed);
}
bool SuperClock::isLinkAudioPublishEnabled() const {
    const SuperClockState* s = state();
    if (!s) return false;
    return (s->flags.load(std::memory_order_relaxed) & SC_FLAG_LINK_AUDIO_PUBLISH) != 0u;
}
void        SuperClock::setPeerName(const char*) {}
const char* SuperClock::peerName() const { return ""; }

std::vector<SuperClock::PeerInfo>          SuperClock::listPeers() const { return {}; }
std::vector<SuperClock::LinkAudioChannel>  SuperClock::listLinkAudioChannels() const { return {}; }

bool SuperClock::addLinkAudioInput(const char*, const char*, uint32_t) { return false; }
void SuperClock::removeLinkAudioInput(const char*, const char*) {}
void SuperClock::clearLinkAudioInputs() {}
std::vector<SuperClock::LinkAudioInputStatus> SuperClock::listLinkAudioInputs() const { return {}; }

bool SuperClock::addLinkAudioSink(const char*, uint32_t, uint32_t) { return false; }
void SuperClock::removeLinkAudioSink(const char*) {}
std::vector<SuperClock::ActiveSinkInfo> SuperClock::listLinkAudioSinks() const { return {}; }

void SuperClock::drainLinkAudioInputsToBuses(float*, uint32_t, uint32_t, uint32_t, uint64_t) {}
void SuperClock::publishAuxSinks(const float*, uint32_t, uint32_t, uint32_t, uint64_t, double) {}
bool SuperClock::publishAudioBlock(const float*, const float*, size_t, uint32_t, uint64_t, double) {
    return false;
}
void SuperClock::publishLinkMetrics(PerformanceMetrics*, double) {}

// ─── Audio-thread NTP ────────────────────────────────────────────────────

double SuperClock::now() const {
    return bitsToDouble(g_last_audio_thread_ntp_bits.load(std::memory_order_acquire));
}

double SuperClock::nowAt(double audioCurrentTime) const {
    const double drift_seconds =
        g_drift_offset ? (g_drift_offset->load(std::memory_order_acquire) / 1000000.0) : 0.0;
    const double ntp_start =
        (g_ntp_start_time && *g_ntp_start_time != 0.0) ? *g_ntp_start_time : 0.0;
    const double global_seconds =
        g_global_offset ? (g_global_offset->load(std::memory_order_relaxed) / 1000.0) : 0.0;
    const double result = audioCurrentTime + ntp_start + drift_seconds + global_seconds;
    g_last_audio_thread_ntp_bits.store(doubleToBits(result), std::memory_order_release);
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

void SuperClock::setFreewheelClock(bool enabled) {
    // No-op: WASM evaluates the SAB time formula in nowAt() and has no
    // headless driver / drift IIR to bypass.
    (void)enabled;
}
