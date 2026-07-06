/*
 * SuperClockNative.cpp — the universal SuperClock composition root.
 *
 * Owns the SuperClockState atomics as a private Impl member and composes the
 * four axes: TimeSource (audio-thread NTP — native WallClock+IIR, or the
 * self-driven worklet clock on WASM/ESP32), MidiTimelines (MIDI follower
 * registry), LinkSession (the Ableton Link clock-sync session, or a thread-free
 * session-of-one when Link is not compiled), and LinkAudioBridge (the Link Audio
 * bus machinery, no-op without Link+synth). This file keeps the SAB binding, the
 * public delegations, and the orchestration that spans more than one axis
 * (setLinkVisibility, the Link-clock-domain audio-host anchor).
 *
 * Every target builds this same composition root: native (all four cells of
 * SYNTH×LINK), the freestanding guard, WASM, and ESP32. On the lean targets the
 * LinkSession / LinkAudioBridge headers supply inline no-ops, and the TimeSource
 * is the self-driven worklet clock. superclock_wasm_init (below, on
 * worklet builds) binds the SAB region + the worklet clock's offset pointers.
 *
 * State coherence: single-atomic-per-field is sufficient. The audio thread reads
 * tempo + isPlaying from Link's captureAudioSessionState (already coherent) and
 * beat_origin / is_playing_at_ntp from individual atomics. There's no
 * torn-multi-field hazard — independent fields, single-atomic loads, no consumer
 * needs a single-instant pair.
 */
#include "SuperClock.h"
#include "native/LinkAudioBridge.h"
#include "native/LinkSession.h"
#include "native/MidiTimelines.h"
#include "native/TimeSource.h"
#include "native/WallClock.h"
#include "shared_memory.h"

#include <cmath>

#if SUPERSONIC_WORKLET_CLOCK
// Resolves to the real emscripten header on WASM and to the NativeShim stub on
// ESP32 (EMSCRIPTEN_KEEPALIVE → nothing there), so superclock_wasm_init keeps
// the WASM export attribute without breaking the embedded build.
#include <emscripten/emscripten.h>
#endif

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

// ─── Impl ────────────────────────────────────────────────────────────────────

struct SuperClock::Impl {
    SuperClockState ownedState;
    // Where the clock state actually lives: the private ownedState until the
    // engine binds it into the shared arena (bindStateToShm), after which it's
    // the SHM SUPERCLOCK_STATE region — same shape as web.
    SuperClockState* boundState{&ownedState};

    // ─── Audio-thread time-source (native WallClock + IIR) ───────────────────
    // The audio-thread NTP clock: one IIR step per callback, freewheel bypass
    // for deterministic offline rendering. The SuperClock now()/nowAt()/
    // updateAudioThreadNTP()/wallNow()/resetAudioThreadTime()/setFreewheelClock()
    // methods are thin delegations to it.
    TimeSource timeSource;

    // Audio-thread anchor for Link Audio receive, kept in Link's clock domain
    // (steady_clock) not the NTP/wall clock — that's the domain beatAtTime()
    // needs. Same sample-counter + slow-IIR scheme as the audio-thread NTP clock.
    double linkAudioHostBaseMicros{0.0};
    bool   linkAudioHostAnchored{false};

    // ─── MIDI follower timelines (in-process registry) ───────────────────────
    // Fixed K-slot registry of midi:<port> timelines, separate from the Link
    // (SuperClockState) timeline. Constructed with the owning SuperClock& so its
    // beat math reads the same clock core; the SuperClock::timeline*/
    // *MidiTimeline* methods are thin delegations to it.
    MidiTimelines midiTimelines;

    // ─── Link clock-sync session ─────────────────────────────────────────────
    // The Ableton Link session (cross-machine tempo/transport/peers) when Link is
    // compiled; a thread-free session-of-one otherwise. The session-mutator and
    // event-callback methods are thin delegations to it. Constructed with the
    // owning SuperClock& (for the SAB mirror) and the MIDI-staleness tick the
    // session's worker drives off the RT thread.
    LinkSession linkSession;

    // Link-Audio audio-bus machinery (publish sink + aux sinks + input
    // subscriptions + RT drain/publish). Real impl when SUPERSONIC_LINK_AUDIO
    // (Link && Synth); no-op otherwise. The SuperClock Link-Audio methods are
    // thin delegations to this.
    LinkAudioBridge linkAudioBridge;

#if SUPERSONIC_LINK_AUDIO
    explicit Impl(SuperClock& clock)
        : midiTimelines(clock),
          linkSession(clock, [this] { midiTimelines.tickMidiStaleness(); }),
          linkAudioBridge(linkSession.linkAudio()) {
        SuperClockState::initDefaults(ownedState);
    }
#else
    explicit Impl(SuperClock& clock)
        : midiTimelines(clock),
          linkSession(clock, [this] { midiTimelines.tickMidiStaleness(); }) {
        SuperClockState::initDefaults(ownedState);
    }
#endif
};

// ─── ctor/dtor + state accessor ───────────────────────────────────────────

SuperClock::SuperClock() : mImpl(std::make_unique<Impl>(*this)) {
    // The session's deferred worker resolves an async enable/disable into a
    // visibility transition; route it back through setLinkVisibility so Link
    // Audio sinks / subs / thread priority all tear down and bring up together.
    mImpl->linkSession.setApplyVisibility(
        [this](LinkVisibility v) { setLinkVisibility(v); });
    // Start the session worker only now that mImpl is assigned. The worker's
    // periodic tick calls back through this SuperClock (linkClockMicros → mImpl),
    // so spawning it inside the LinkSession member ctor — while make_unique is
    // still writing mImpl — is a data race on mImpl with no happens-before edge.
    mImpl->linkSession.startWorker();
}

SuperClock::~SuperClock() {
    // Join the session worker before any Impl member it reaches is destroyed.
    // Member reverse-destruction tears down linkAudioBridge (and would expose
    // midiTimelines) while linkSession — and its still-live worker — has not yet
    // been reached, so the worker could run applyVisibility/the staleness tick
    // against a half-destroyed Impl. Stopping it first closes that window.
    mImpl->linkSession.stopWorker();
}

void SuperClock::stopBackgroundWork() {
    mImpl->linkSession.stopWorker();
}

SuperClockState*       SuperClock::state()       { return mImpl->boundState; }
const SuperClockState* SuperClock::state() const { return mImpl->boundState; }

// Move the clock state into the shared arena region so the native SHM has the
// same shape as web. Copies the current (pre-bind) state across, then repoints.
// Called once at engine init, before the audio thread starts — no concurrency.
void SuperClock::bindStateToShm(SuperClockState* region) {
    if (!region || region == mImpl->boundState) return;
    const SuperClockState* src = mImpl->boundState;
    region->bpm.store(src->bpm.load(std::memory_order_relaxed), std::memory_order_relaxed);
    region->beat_origin_ntp.store(src->beat_origin_ntp.load(std::memory_order_relaxed), std::memory_order_relaxed);
    region->is_playing_at_ntp.store(src->is_playing_at_ntp.load(std::memory_order_relaxed), std::memory_order_relaxed);
    region->is_playing.store(src->is_playing.load(std::memory_order_relaxed), std::memory_order_relaxed);
    region->flags.store(src->flags.load(std::memory_order_relaxed), std::memory_order_relaxed);
    mImpl->boundState = region;
}

#if SUPERSONIC_WORKLET_CLOCK

// ─── Worklet (WASM/ESP32) boot binding ──────────────────────────────────────
// The worklet clock evaluates the SAB time formula from these offset pointers.
void SuperClock::bindWorkletClock(const double* ntpStartTime,
                                  const std::atomic<int32_t>* driftOffset,
                                  const std::atomic<int32_t>* globalOffset) {
    mImpl->timeSource.bindOffsets(ntpStartTime, driftOffset, globalOffset);
}

extern "C" {

// The WASM host's boot hook. init_memory publishes the composition-root instance
// to g_active_superclock, then calls this with the SAB SUPERCLOCK_STATE region
// and the three SAB offset pointers. We bind both onto that instance: the state
// (so SuperClock::state() reads/writes the SAB) and the worklet clock offsets
// (so nowAt() evaluates the SAB time formula). Signature is fixed — kept stable
// across the SuperClock decomposition so the WASM/JS side needs no change.
EMSCRIPTEN_KEEPALIVE
void superclock_wasm_init(SuperClockState* superclock_state,
                          const double* ntp_start_time_ptr,
                          const std::atomic<int32_t>* drift_offset_ptr,
                          const std::atomic<int32_t>* global_offset_ptr) {
    SuperClock* clock = g_active_superclock.load(std::memory_order_acquire);
    if (!clock) return;
    clock->bindStateToShm(superclock_state);
    clock->bindWorkletClock(ntp_start_time_ptr, drift_offset_ptr, global_offset_ptr);
}

}  // extern "C"

#endif  // SUPERSONIC_WORKLET_CLOCK

// ─── MIDI follower timelines (native) ──────────────────────────────────────
// The registry lives in MidiTimelines (constructed with this SuperClock as its
// clock core). These are thin delegations; id 0 = Link is handled inside.

int SuperClock::claimMidiTimeline(const char* normalized, const char* raw) {
    return mImpl->midiTimelines.claimMidiTimeline(normalized, raw);
}

void SuperClock::freeMidiTimeline(int id) {
    mImpl->midiTimelines.freeMidiTimeline(id);
}

int SuperClock::resolveTimeline(const char* name) const {
    return mImpl->midiTimelines.resolveTimeline(name);
}

int SuperClock::resolveOrClaimTimeline(const char* name) {
    return mImpl->midiTimelines.resolveOrClaimTimeline(name);
}

void SuperClock::setMidiTimelineTempo(int id, double bpm) {
    mImpl->midiTimelines.setMidiTimelineTempo(id, bpm);
}

void SuperClock::midiTimelinePulse(int id, uint64_t tsUs) {
    mImpl->midiTimelines.midiTimelinePulse(id, tsUs);
}

void SuperClock::setMidiTimelineTransport(int id, int kind, double beat) {
    mImpl->midiTimelines.setMidiTimelineTransport(id, kind, beat);
}

void SuperClock::tickMidiStaleness() {
    mImpl->midiTimelines.tickMidiStaleness();
}

double SuperClock::timelineBpm(int id) const {
    return mImpl->midiTimelines.timelineBpm(id);
}

bool SuperClock::timelineIsPlaying(int id) const {
    return mImpl->midiTimelines.timelineIsPlaying(id);
}

bool SuperClock::timelineIsAnchored(int id) const {
    return mImpl->midiTimelines.timelineIsAnchored(id);
}

int64_t SuperClock::timelineTimeForIsPlayingMicros(int id) const {
    return mImpl->midiTimelines.timelineTimeForIsPlayingMicros(id);
}

double SuperClock::timelineBeatAtLinkTime(int id, int64_t timeMicros, double quantum) const {
    return mImpl->midiTimelines.timelineBeatAtLinkTime(id, timeMicros, quantum);
}

double SuperClock::timelinePhaseAtLinkTime(int id, int64_t timeMicros, double quantum) const {
    return mImpl->midiTimelines.timelinePhaseAtLinkTime(id, timeMicros, quantum);
}

int64_t SuperClock::timelineTimeAtBeatLinkMicros(int id, double beat, double quantum) const {
    return mImpl->midiTimelines.timelineTimeAtBeatLinkMicros(id, beat, quantum);
}

std::vector<SuperClock::TimelineInfo> SuperClock::listTimelines() const {
    return mImpl->midiTimelines.listTimelines();
}

void SuperClock::setTimelinesChangedCallback(std::function<void()> cb) {
    mImpl->midiTimelines.setTimelinesChangedCallback(std::move(cb));
}

// ─── Session mutators (app-thread) ─────────────────────────────────────────
// Thin delegations to LinkSession, which keeps the SuperClockState SAB mirror in
// sync (writes through Link + its callbacks on the Ableton path; direct SAB
// writes on the session-of-one path).

void SuperClock::setBpm(double bpm, double atNtpSeconds) {
    (void)atNtpSeconds;
    // Guard div-by-zero in beat math (timeAtBeat / requestBeatAtTime).
    if (!(bpm >= 1.0)) bpm = 1.0;
    mImpl->linkSession.setBpm(bpm);
}

void SuperClock::setIsPlaying(bool playing, double atNtpSeconds) {
    mImpl->linkSession.setIsPlaying(playing, atNtpSeconds);
}

void SuperClock::setLinkEnabled(bool enabled) {
    // Route through setLinkVisibility so Link Audio sinks / subs / thread
    // priority all tear down and bring up together.
    if (enabled) {
        if (getLinkVisibility() == LinkVisibility::Off) {
            setLinkVisibility(mImpl->linkSession.lastNonOffVisibility());
        }
    } else {
        setLinkVisibility(LinkVisibility::Off);
    }
}

void SuperClock::setStartStopSyncEnabled(bool enabled) {
    mImpl->linkSession.setStartStopSyncEnabled(enabled);
}

void SuperClock::requestBeatAtTime(double beat, double atNtpSeconds, double quantum) {
    mImpl->linkSession.requestBeatAtTime(beat, atNtpSeconds, quantum);
}

void SuperClock::forceBeatAtTime(double beat, double atNtpSeconds, double quantum) {
    mImpl->linkSession.forceBeatAtTime(beat, atNtpSeconds, quantum);
}

void SuperClock::requestSetLinkEnabledAsync(bool enabled) {
    mImpl->linkSession.requestSetLinkEnabledAsync(enabled);
}

void* SuperClock::audioThreadLinkAudioPtr() {
    return mImpl->linkSession.audioThreadLinkAudioPtr();
}

// ─── Getters (app-thread) ──────────────────────────────────────────────────

bool SuperClock::isLinkEnabled() const {
    return mImpl->linkSession.isEnabled();
}

bool SuperClock::isStartStopSyncEnabled() const {
    return mImpl->linkSession.isStartStopSyncEnabled();
}

size_t SuperClock::numPeers() const {
    return mImpl->linkSession.numPeers();
}

// ─── Link-clock-domain RPC ──────────────────────────────────────────────────

int64_t SuperClock::linkClockMicros() const {
    return mImpl->linkSession.clockMicros();
}

int64_t SuperClock::ntpNowMicros() const {
    return (int64_t)std::llround(wallClockNTP() * 1e6);
}

int64_t SuperClock::linkMicrosToNtpMicros(int64_t linkMicros) const {
    // Offset sampled fresh in-process: wallClockNTP() and the Link clock are
    // read microseconds apart, so there's no drift to accumulate and no per-boot
    // epoch to track on the client. Recomputed every call, so it's always
    // current — including right after a wake, once the Link clock resumes.
    return linkMicros + (ntpNowMicros() - linkClockMicros());
}

int64_t SuperClock::ntpMicrosToLinkMicros(int64_t ntpMicros) const {
    return ntpMicros - (ntpNowMicros() - linkClockMicros());
}

int64_t SuperClock::timeForIsPlayingMicros() const {
    return mImpl->linkSession.timeForIsPlayingMicros();
}

double SuperClock::beatAtLinkTime(int64_t timeMicros, double quantum) const {
    return mImpl->linkSession.beatAtLinkTime(timeMicros, quantum);
}

double SuperClock::phaseAtLinkTime(int64_t timeMicros, double quantum) const {
    return mImpl->linkSession.phaseAtLinkTime(timeMicros, quantum);
}

int64_t SuperClock::timeAtBeatLinkMicros(double beat, double quantum) const {
    return mImpl->linkSession.timeAtBeatLinkMicros(beat, quantum);
}

// Link micros ↔ NTP seconds: a plain 1e6 scale. The Link RPC methods above work
// directly in Link micros and don't route through these.
double SuperClock::linkMicrosToNtpSeconds(int64_t linkMicros) const {
    return static_cast<double>(linkMicros) / 1'000'000.0;
}
int64_t SuperClock::ntpSecondsToLinkMicros(double ntpSeconds) const {
    return static_cast<int64_t>(ntpSeconds * 1'000'000.0);
}

// ─── Link event callbacks ────────────────────────────────────────────────────

void SuperClock::setTempoChangedCallback(std::function<void(double)> cb) {
    mImpl->linkSession.setTempoChangedCallback(std::move(cb));
}

void SuperClock::setNumPeersChangedCallback(std::function<void(std::size_t)> cb) {
    mImpl->linkSession.setNumPeersChangedCallback(std::move(cb));
}

void SuperClock::setStartStopChangedCallback(
    std::function<void(bool, int64_t)> cb) {
    mImpl->linkSession.setStartStopChangedCallback(std::move(cb));
}

// ─── Link Audio: visibility / publish / peer name ───────────────────────────
// setLinkVisibility orchestrates the two Link-coupled axes: it drives the
// LinkSession enable/visibility primitives and the LinkAudioBridge sink/sub
// machinery in the order required for a clean substrate transition.

void SuperClock::setLinkVisibility(LinkVisibility v) {
    if (v == getLinkVisibility()) return;

    if (v != LinkVisibility::Off) mImpl->linkSession.setLastNonOffVisibility(v);

    // Tear down then bring up. Disabling Link forces immediate gateway teardown
    // so the loopback flag transition is instant — no ~5 s rescan window. Input
    // subs / aux sinks / main sink bind to the current Link substrate
    // (session-scoped channelIds), so clear them BEFORE the session disables —
    // app must re-add after the next non-Off transition.
    mImpl->linkAudioBridge.resetForVisibilityChange();
    mImpl->linkSession.prepareDisable();

    // Interface-filter flag picked up by the next enable.
    mImpl->linkSession.setLoopbackOnly(v == LinkVisibility::LoopbackOnly);

    SuperClockState* s = state();
    if (v == LinkVisibility::Off) {
        if (s) s->flags.fetch_and(~SC_FLAG_LINK_ENABLED, std::memory_order_relaxed);
        return;
    }

    // Flag-flip before bring-up so observers never see "Link on, flag off"
    // mid-transition.
    if (s) s->flags.fetch_or(SC_FLAG_LINK_ENABLED, std::memory_order_relaxed);

    // Bring up in order: enable (ScanIpIfAddrs respects loopback flag), raise
    // thread priority, enableLinkAudio unconditionally (so we can observe peer
    // channels even in receive-only mode), then sink only if publishing was
    // requested.
    mImpl->linkSession.enableWithPriority();
    mImpl->linkAudioBridge.ensureMainSink();
}

SuperClock::LinkVisibility SuperClock::getLinkVisibility() const {
    return mImpl->linkSession.getVisibility();
}

void SuperClock::setLinkAudioPublish(bool publish) {
    // SAB flag is kept here (the bridge has no SuperClockState handle) for
    // cross-build layout parity. The sink machinery — reflected immediately when
    // the Link mesh is up, else created by the next setLinkVisibility(non-Off) →
    // ensureMainSink — lives in the bridge (no-op when no audio bus).
    if (publish) state()->flags.fetch_or(SC_FLAG_LINK_AUDIO_PUBLISH,  std::memory_order_relaxed);
    else         state()->flags.fetch_and(~SC_FLAG_LINK_AUDIO_PUBLISH, std::memory_order_relaxed);
    mImpl->linkAudioBridge.setPublishEnabled(publish, mImpl->linkSession.isEnabled());
}

bool SuperClock::isLinkAudioPublishEnabled() const {
    return mImpl->linkAudioBridge.isPublishEnabled();
}

void SuperClock::setPeerName(const char* name) {
    mImpl->linkSession.setPeerName(name);
}

const char* SuperClock::peerName() const {
    return mImpl->linkSession.peerName();
}

// ─── Link Audio: peer + channel discovery ───────────────────────────────────

std::vector<SuperClock::PeerInfo> SuperClock::listPeers() const {
    return mImpl->linkSession.listPeers();
}

std::vector<SuperClock::LinkAudioChannel>
SuperClock::listLinkAudioChannels() const {
    return mImpl->linkAudioBridge.listChannels();
}

// ─── Link Audio: input subscription ─────────────────────────────────────────

bool SuperClock::addLinkAudioInput(const char* peerName,
                                    const char* channelName,
                                    uint32_t busIdx) {
    return mImpl->linkAudioBridge.addInput(peerName, channelName, busIdx);
}

void SuperClock::removeLinkAudioInput(const char* peerName,
                                       const char* channelName) {
    mImpl->linkAudioBridge.removeInput(peerName, channelName);
}

void SuperClock::clearLinkAudioInputs() {
    mImpl->linkAudioBridge.clearInputs();
}

bool SuperClock::setLinkAudioInputLatencySeconds(const char* peerName,
                                                  const char* channelName,
                                                  double seconds) {
    return mImpl->linkAudioBridge.setInputLatencySeconds(peerName, channelName, seconds);
}

std::vector<SuperClock::LinkAudioInputStatus>
SuperClock::listLinkAudioInputs() const {
    return mImpl->linkAudioBridge.listInputs();
}

// ─── Link Audio: auxiliary sinks ────────────────────────────────────────────

bool SuperClock::addLinkAudioSink(const char* name,
                                   uint32_t busIdx,
                                   uint32_t numChannels) {
    return mImpl->linkAudioBridge.addSink(name, busIdx, numChannels);
}

void SuperClock::removeLinkAudioSink(const char* name) {
    mImpl->linkAudioBridge.removeSink(name);
}

std::vector<SuperClock::ActiveSinkInfo> SuperClock::listLinkAudioSinks() const {
    return mImpl->linkAudioBridge.listSinks();
}

// ─── Audio-thread: publish + drain (RT-safe) ────────────────────────────────

void SuperClock::publishAuxSinks(const float* busPool,
                                  uint32_t blockSize,
                                  uint32_t numBuses,
                                  uint32_t sampleRate,
                                  uint64_t hostMicrosForBufferBegin,
                                  double quantum) {
    mImpl->linkAudioBridge.publishAuxSinks(busPool, blockSize, numBuses,
                                           sampleRate, hostMicrosForBufferBegin,
                                           quantum);
}

void SuperClock::drainLinkAudioInputsToBuses(float* busPool,
                                              uint32_t blockSize,
                                              uint32_t numBuses,
                                              uint32_t sampleRate,
                                              uint64_t hostMicrosForBufferBegin) {
    mImpl->linkAudioBridge.drainInputsToBuses(busPool, blockSize, numBuses,
                                              sampleRate, hostMicrosForBufferBegin);
}

bool SuperClock::publishAudioBlock(const float* leftChannel,
                                    const float* rightChannel,
                                    size_t numFrames,
                                    uint32_t sampleRate,
                                    uint64_t hostMicrosForBufferBegin,
                                    double quantum) {
    return mImpl->linkAudioBridge.publishAudioBlock(leftChannel, rightChannel,
                                                    numFrames, sampleRate,
                                                    hostMicrosForBufferBegin,
                                                    quantum);
}

void SuperClock::publishLinkMetrics(PerformanceMetrics* m, double quantum) {
    if (!m) return;

    // Link clock readouts — always written so the LINK card is live even with no
    // Link Audio subscriptions. No-op on a session-of-one (no Link).
    mImpl->linkSession.publishLinkClockMetrics(m, quantum);

    // Link Audio health (no-op bridge returns defaults / false try-reads, so
    // these fields stay at their last value on a Link-only / no-synth build).
    auto& bridge = mImpl->linkAudioBridge;
    m->link_audio_publish.store(bridge.isPublishEnabled() ? 1u : 0u,
                                std::memory_order_relaxed);
    uint32_t sinkCount = 0;
    if (bridge.tryReadSinkCount(sinkCount))
        m->link_audio_sinks.store(sinkCount, std::memory_order_relaxed);

    m->link_audio_underruns.store(bridge.underruns(), std::memory_order_relaxed);
    LinkAudioBridge::InputHealth health;
    if (bridge.tryReadInputHealth(health)) {
        m->link_audio_in_channels.store(health.inChannels, std::memory_order_relaxed);
        m->link_audio_stream_rate.store(health.streamRate, std::memory_order_relaxed);
        m->link_audio_drift_ppm.store(health.driftPpm, std::memory_order_relaxed);
        m->link_audio_buffered_ms.store(health.bufferedMs, std::memory_order_relaxed);
    }
}

// ─── Audio-thread NTP (delegated to TimeSource) ─────────────────────────────
// Thin delegations to the native time-source (WallClock + IIR). RT-safe;
// link-time-concrete, no vtable on the audio-thread path.

double SuperClock::now() const {
    return mImpl->timeSource.now();
}

double SuperClock::nowAt(double audioCurrentTime) const {
    return mImpl->timeSource.nowAt(audioCurrentTime);
}

double SuperClock::wallNow() const {
    return mImpl->timeSource.wallNow();
}

double SuperClock::updateAudioThreadNTP(double samplePosition,
                                         double sampleRate,
                                         double audioCurrentTime) {
    return mImpl->timeSource.updateAudioThreadNTP(samplePosition, sampleRate,
                                                  audioCurrentTime);
}

void SuperClock::resetAudioThreadTime(double samplePosition, double sampleRate) {
    mImpl->timeSource.resetAudioThreadTime(samplePosition, sampleRate);
    // Re-anchor the Link-domain audio host clock on the next call.
    mImpl->linkAudioHostAnchored = false;
}

// Drift-corrected host time (Link clock domain, µs) for the Link Audio receive
// resampler: derived from the monotonic sample counter (jitter-free) and
// slow-IIR-corrected toward the Link clock micros. Reading the Link clock at
// audio-thread wake is jittery; the IIR rejects that while tracking real drift.
// 0 on a no-Link build (the session reports a zero clock).
int64_t SuperClock::linkAudioHostMicros(double samplePosition, double sampleRate) {
    const int64_t linkClock = mImpl->linkSession.linkClockMicrosRaw();
    if (linkClock == 0) return 0;
    if (sampleRate <= 0.0) return linkClock;
    const double sampleOffsetMicros = (samplePosition / sampleRate) * 1e6;
    const double linkNow = static_cast<double>(linkClock);

    if (!mImpl->linkAudioHostAnchored) {
        mImpl->linkAudioHostBaseMicros = linkNow - sampleOffsetMicros;
        mImpl->linkAudioHostAnchored = true;
    } else {
        // baseMicros += (linkNow - (base + sampleOffset)) * 0.01
        const double drift =
            linkNow - (mImpl->linkAudioHostBaseMicros + sampleOffsetMicros);
        mImpl->linkAudioHostBaseMicros += drift * 0.01;
    }
    return static_cast<int64_t>(mImpl->linkAudioHostBaseMicros + sampleOffsetMicros);
}

void SuperClock::setFreewheelClock(bool enabled) {
    mImpl->timeSource.setFreewheelClock(enabled);
}
