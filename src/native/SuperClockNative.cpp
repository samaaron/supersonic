/*
 * SuperClockNative.cpp — native backend for SuperClock.
 *
 * Owns the SuperClockState atomics as a private Impl member. On builds
 * with SUPERSONIC_LINK defined, Impl additionally wraps an
 * ableton::LinkAudio instance; mutators delegate to Link's API, and
 * tempo/transport callbacks fired by Link mirror back into the atomics
 * so all snapshot readers (including no-Link / WASM) see the same data
 * shape.
 *
 * State coherence: single-atomic-per-field is sufficient. The audio
 * thread reads tempo + isPlaying from Link's captureAudioSessionState
 * (already coherent) and beat_origin / is_playing_at_ntp from
 * individual atomics. There's no torn-multi-field hazard — independent
 * fields, single-atomic loads, no consumer needs a single-instant pair.
 */
#include "SuperClock.h"
#include "native/WallClock.h"
#include "shared_memory.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <string>
#include <vector>

#ifdef SUPERSONIC_LINK
// LinkAudio.hpp must be used INSTEAD of Link.hpp ("LinkAudio and Link
// should not be used simultaneously" per Ableton's docs).
#include <ableton/LinkAudio.hpp>
#include <ableton/util/FloatIntConversion.hpp>
#include "vendor/LinkAudioInputRenderer.hpp"
#include <algorithm>
#include <optional>

// Loopback-only interface filtering. The flag is the one our
// link-loopback-mode.patch adds to the platform's ScanIpIfAddrs.hpp;
// setLinkVisibility toggles it and Link's InterfaceScanner constrains
// discovery/multicast/unicast to loopback on its next scan. Same flag on
// both platforms — keep the include and the accessor switched in lockstep.
#if defined(_WIN32)
#include <ableton/platforms/windows/ScanIpIfAddrs.hpp>
#else
#include <ableton/platforms/posix/ScanIpIfAddrs.hpp>
#endif
namespace {
inline std::atomic<bool>& linkLoopbackOnlyFlag() {
#if defined(_WIN32)
    return ableton::platforms::windows::loopbackOnly();
#else
    return ableton::platforms::posix::loopbackOnly();
#endif
}
}  // namespace
#endif

// touch_audio_bus is declared by JuceAudioCallback.h (extern "C"
// pass-through to audio_processor.cpp). Forward-declare here so we
// don't drag the whole header in. drain runs BEFORE process_audio's
// mBufCounter++ so we call the _for_next_block variant to land in
// scsynth's In.ar visibility window for the upcoming block.
extern "C" void touch_audio_bus(uint32_t busIdx);
extern "C" void touch_audio_bus_for_next_block(uint32_t busIdx);

using supersonic::doubleToBits;
using supersonic::bitsToDouble;

// ─── Impl ────────────────────────────────────────────────────────────────

struct SuperClock::Impl {
    SuperClockState ownedState;
    // Where the clock state actually lives: the private ownedState until the
    // engine binds it into the shared arena (bindStateToShm), after which it's
    // the SHM SUPERCLOCK_STATE region — same shape as web.
    SuperClockState* boundState{&ownedState};

    // IIR-smoothed audio-thread NTP. Bit-pattern in uint64 because
    // std::atomic<double> isn't lock-free everywhere.
    std::atomic<uint64_t> baseNTPBits{0};
    std::atomic<uint64_t> currentAudioThreadNTPBits{0};
    // When true, updateAudioThreadNTP skips the wall-clock drift IIR and
    // returns a pure sample-derived NTP (deterministic; see setFreewheelClock).
    std::atomic<bool>     freewheelClock{false};

    // Audio-thread anchor for Link Audio receive, kept in Link's clock domain
    // (steady_clock) not the NTP/wall clock — that's the domain beatAtTime()
    // needs. Same sample-counter + slow-IIR scheme as baseNTPBits.
    double linkAudioHostBaseMicros{0.0};
    bool   linkAudioHostAnchored{false};

#ifdef SUPERSONIC_LINK
    // Link starts disabled; setLinkVisibility(non-Off) flips peer
    // discovery on. While disabled the instance still hosts session
    // state — we read tempo/transport from it even with no peers.
    ableton::LinkAudio link{120.0, "SuperSonic"};

    // RT priority for Link's network thread. Without it the receiver
    // hears periodic packet-burst pulsing when the OS preempts the
    // thread (mirrors Ableton's linkaudiohut example).
    ableton::link::platform::ThreadPriority threadPriority{};

    // Backing storage for the peer name we advertise. Link::setName
    // takes by value; we keep the C-string stable for peerName().
    std::string peerNameCache{"SuperSonic"};

    // Audio-channel name (separate from peer name — Live displays one
    // row per channel under each peer). "Main" matches Live's own
    // convention for its primary output.
    std::string channelNameCache{"Main"};

    // Up to 2048-frame stereo blocks; scsynth normally runs at 128.
    static constexpr size_t kSinkMaxSamples = 4096;
    std::optional<ableton::LinkAudioSink> sink;
    // Serialises sink reset/emplace against the audio thread's
    // publishAudioBlock dereference (audio thread uses try_lock).
    mutable std::mutex sinkMutex;

    // Audio-publish gate. False = don't create sink even if Link mesh
    // is up. App must explicitly setLinkAudioPublish(true).
    bool audioPublishEnabled{false};

    // Last non-Off visibility, restored by setLinkEnabled(false→true).
    // Defaults to LoopbackOnly: bare enable on a fresh SuperClock
    // stays loopback-only until the caller explicitly opts into
    // NetworkWide via setLinkVisibility.
    LinkVisibility lastNonOffVisibility{LinkVisibility::LoopbackOnly};

    // One subscription per Link channel. Multiple are active
    // concurrently; (peerName, channelName) is the replacement key.
    // Each subscription owns one bus.
    struct InputSubscription {
        std::unique_ptr<supersonic_link::LinkAudioInputRenderer<ableton::LinkAudio>> renderer;
        uint32_t    busIdx{0};
        std::string peerName;
        std::string channelName;
        // Distinguishes a re-arm of the same channel from a peer
        // rejoin that publishes a new id. Same-id replacements reuse
        // the existing renderer so diagnostic counters survive.
        ableton::ChannelId channelId{};
    };
    std::vector<InputSubscription> inputSubs;
    // Lock-free fast path for the empty case.
    std::atomic<size_t>            inputSubCount{0};
    // Serialises inputSubs mutation against the audio thread's
    // drainLinkAudioInputsToBuses (try_lock) and the app-thread
    // listLinkAudioInputs (blocking).
    mutable std::mutex             inputSubMutex;

    // Auxiliary sinks bound to user-chosen bus ranges. Mutex covers the
    // vector and the per-entry hasSubscriber flag; audio thread uses
    // try_lock and skips the block on contention.
    struct ActiveSink {
        std::string            name;
        uint32_t               busIdx;
        uint32_t               numChannels;
        ableton::LinkAudioSink sink;
        bool                   hasSubscriber{false};
    };
    mutable std::mutex      auxSinksMutex;
    std::vector<ActiveSink> auxSinks;
    // Lock-free fast path for the empty case.
    std::atomic<size_t>     auxSinkCount{0};

    // Audio-thread scratch for drainLinkAudioInputsToBuses. Held on
    // Impl to avoid 64 KiB of stack per call (some Windows ASIO
    // threads have <128 KiB stacks). Single-thread access.
    static constexpr size_t kDrainScratchFrames = 4096;
    double drainScratchL[kDrainScratchFrames]{};
    double drainScratchR[kDrainScratchFrames]{};

    // Deferred Link enable/disable for audio-thread callers.
    // -1 = no request, 0 = disable, 1 = enable.
    std::thread             deferredWorker;
    std::mutex              deferredMtx;
    std::condition_variable deferredCv;
    std::atomic<int>        deferredLinkEnableReq{-1};
    std::atomic<bool>       deferredQuit{false};

    // Cumulative Link Audio receive underruns (a block the renderer couldn't
    // fully fill). Bumped in drainLinkAudioInputsToBuses, mirrored to metrics.
    std::atomic<uint32_t>   linkAudioUnderruns{0};
#endif

    Impl() { SuperClockState::initDefaults(ownedState); }
};

namespace {

#ifdef SUPERSONIC_LINK
// Render an 8-byte Link NodeId/PeerId/ChannelId as 16 lowercase hex.
template <typename Bytes>
inline std::string bytesToHex16(const Bytes& bytes) {
    char buf[17];
    for (size_t k = 0; k < bytes.size(); ++k) {
        std::snprintf(buf + 2 * k, 3, "%02x", bytes[k]);
    }
    return std::string(buf, 16);
}

// Use the caller-supplied audio-framework timestamp if available;
// otherwise fall back to link.clock().micros() (jittery; testing only).
inline std::chrono::microseconds hostMicrosOrNow(
    uint64_t hostMicros, const ableton::LinkAudio& link) {
    return hostMicros > 0
        ? std::chrono::microseconds{static_cast<int64_t>(hostMicros)}
        : link.clock().micros();
}

// Interleave two float channels (R nullable for mono) into one int16
// buffer with Link's saturating float→int16 conversion.
inline void interleaveFloatToInt16(const float* L, const float* R,
                                    int16_t* dst, size_t numFrames) {
    if (R) {
        for (size_t i = 0; i < numFrames; ++i) {
            dst[2 * i]     = ableton::util::floatToInt16(L[i]);
            dst[2 * i + 1] = ableton::util::floatToInt16(R[i]);
        }
    } else {
        for (size_t i = 0; i < numFrames; ++i) {
            dst[i] = ableton::util::floatToInt16(L[i]);
        }
    }
}
#endif  // SUPERSONIC_LINK

}  // namespace

// ─── ctor/dtor + state accessor ─────────────────────────────────────────

SuperClock::SuperClock() : mImpl(std::make_unique<Impl>()) {
#ifdef SUPERSONIC_LINK
    mImpl->deferredWorker = std::thread([this] {
        for (;;) {
            std::unique_lock<std::mutex> lk(mImpl->deferredMtx);
            mImpl->deferredCv.wait(lk, [this] {
                return mImpl->deferredQuit.load(std::memory_order_acquire)
                    || mImpl->deferredLinkEnableReq.load(
                           std::memory_order_acquire) != -1;
            });
            if (mImpl->deferredQuit.load(std::memory_order_acquire)) return;
            const int req = mImpl->deferredLinkEnableReq.exchange(
                -1, std::memory_order_acq_rel);
            lk.unlock();
            if (req == 1)      setLinkEnabled(true);
            else if (req == 0) setLinkEnabled(false);
        }
    });
#endif
}

SuperClock::~SuperClock() {
#ifdef SUPERSONIC_LINK
    // Set the quit flag under the same mutex the worker waits on: a notify_all()
    // issued in the window between the worker's predicate check and its park in
    // wait() would otherwise be lost, leaving the worker asleep forever and this
    // join() hung.
    {
        std::lock_guard<std::mutex> lk(mImpl->deferredMtx);
        mImpl->deferredQuit.store(true, std::memory_order_release);
    }
    mImpl->deferredCv.notify_all();
    if (mImpl->deferredWorker.joinable()) mImpl->deferredWorker.join();
#endif
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

// ─── Mutators (app-thread) ──────────────────────────────────────────────

void SuperClock::setBpm(double bpm, double atNtpSeconds) {
    (void)atNtpSeconds;
    // Guard div-by-zero in beat math (timeAtBeat / requestBeatAtTime).
    if (!(bpm >= 1.0)) bpm = 1.0;
#ifdef SUPERSONIC_LINK
    auto st = mImpl->link.captureAppSessionState();
    st.setTempo(bpm, mImpl->link.clock().micros());
    mImpl->link.commitAppSessionState(st);
#endif
    // Mirror the input. Link's tempo callback re-syncs the mirror to
    // Link's converged value when peers exist.
    SuperClockState* s = state();
    if (!s) return;
    s->bpm.store(doubleToBits(bpm), std::memory_order_relaxed);
}

void SuperClock::setIsPlaying(bool playing, double atNtpSeconds) {
#ifdef SUPERSONIC_LINK
    auto st = mImpl->link.captureAppSessionState();
    st.setIsPlaying(playing, mImpl->link.clock().micros());
    mImpl->link.commitAppSessionState(st);
#endif
    SuperClockState* s = state();
    if (!s) return;
    s->is_playing_at_ntp.store(doubleToBits(atNtpSeconds), std::memory_order_relaxed);
    s->is_playing.store(playing ? 1u : 0u, std::memory_order_relaxed);
}

void SuperClock::setLinkEnabled(bool enabled) {
#ifdef SUPERSONIC_LINK
    // Route through setLinkVisibility so Link Audio sinks / subs /
    // thread priority all tear down and bring up together.
    if (enabled) {
        if (getLinkVisibility() == LinkVisibility::Off) {
            setLinkVisibility(mImpl->lastNonOffVisibility);
        }
    } else {
        setLinkVisibility(LinkVisibility::Off);
    }
#else
    SuperClockState* s = state();
    if (!s) return;
    if (enabled) s->flags.fetch_or(SC_FLAG_LINK_ENABLED,  std::memory_order_relaxed);
    else         s->flags.fetch_and(~SC_FLAG_LINK_ENABLED, std::memory_order_relaxed);
#endif
}

void SuperClock::setStartStopSyncEnabled(bool enabled) {
#ifdef SUPERSONIC_LINK
    mImpl->link.enableStartStopSync(enabled);
#endif
    SuperClockState* s = state();
    if (!s) return;
    if (enabled) s->flags.fetch_or(SC_FLAG_START_STOP_SYNC,  std::memory_order_relaxed);
    else         s->flags.fetch_and(~SC_FLAG_START_STOP_SYNC, std::memory_order_relaxed);
}

void SuperClock::requestBeatAtTime(double beat, double atNtpSeconds, double quantum) {
#ifdef SUPERSONIC_LINK
    auto st = mImpl->link.captureAppSessionState();
    st.requestBeatAtTime(beat, mImpl->link.clock().micros(), quantum);
    double tempo = st.tempo();
    if (!(tempo >= 1.0)) tempo = 1.0;
    mImpl->link.commitAppSessionState(st);
    // Mirror the implied NTP beat origin so snapshot's beatAtTime math
    // works against NTP (Link stores in mClock domain, not NTP).
    const double newOrigin = atNtpSeconds - beat * 60.0 / tempo;
    SuperClockState* s = state();
    if (!s) return;
    s->beat_origin_ntp.store(doubleToBits(newOrigin), std::memory_order_relaxed);
#else
    (void)quantum;
    SuperClockState* s = state();
    if (!s) return;
    const double bpm = bitsToDouble(s->bpm.load(std::memory_order_relaxed));
    const double newOrigin = atNtpSeconds - beat * 60.0 / bpm;
    s->beat_origin_ntp.store(doubleToBits(newOrigin), std::memory_order_relaxed);
#endif
}

void SuperClock::forceBeatAtTime(double beat, double atNtpSeconds, double quantum) {
#ifdef SUPERSONIC_LINK
    auto st = mImpl->link.captureAppSessionState();
    st.forceBeatAtTime(beat, mImpl->link.clock().micros(), quantum);
    double tempo = st.tempo();
    if (!(tempo >= 1.0)) tempo = 1.0;
    mImpl->link.commitAppSessionState(st);
    const double newOrigin = atNtpSeconds - beat * 60.0 / tempo;
    SuperClockState* s = state();
    if (!s) return;
    s->beat_origin_ntp.store(doubleToBits(newOrigin), std::memory_order_relaxed);
#else
    // No peers in session-of-one — identical to requestBeatAtTime.
    requestBeatAtTime(beat, atNtpSeconds, quantum);
#endif
}

// ─── Getters (app-thread) ───────────────────────────────────────────────

void SuperClock::requestSetLinkEnabledAsync(bool enabled) {
#ifdef SUPERSONIC_LINK
    // Under the worker's mutex (see ~SuperClock) so the request can't be lost to
    // a notify that races the worker's park in wait().
    {
        std::lock_guard<std::mutex> lk(mImpl->deferredMtx);
        mImpl->deferredLinkEnableReq.store(enabled ? 1 : 0,
                                           std::memory_order_release);
    }
    mImpl->deferredCv.notify_all();
#else
    (void)enabled;
#endif
}

void* SuperClock::audioThreadLinkAudioPtr() {
#ifdef SUPERSONIC_LINK
    return &mImpl->link;
#else
    return nullptr;
#endif
}

bool SuperClock::isLinkEnabled() const {
#ifdef SUPERSONIC_LINK
    return mImpl->link.isEnabled();
#else
    // Link isn't compiled in — report capability, not the SAB flag
    // mirror (which only exists for cross-build SAB layout parity).
    return false;
#endif
}

bool SuperClock::isStartStopSyncEnabled() const {
#ifdef SUPERSONIC_LINK
    return mImpl->link.isStartStopSyncEnabled();
#else
    return false;
#endif
}

size_t SuperClock::numPeers() const {
#ifdef SUPERSONIC_LINK
    return mImpl->link.numPeers();
#else
    return 0;
#endif
}

// ─── Link-clock-domain RPC ──────────────────────────────────────────────

int64_t SuperClock::linkClockMicros() const {
#ifdef SUPERSONIC_LINK
    return mImpl->link.clock().micros().count();
#else
    // No Link → return steady_clock micros so the value is monotonic
    // for callers that just need a "now" timestamp in the same domain
    // as future linkClockMicros calls.
    return std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
#endif
}

int64_t SuperClock::timeForIsPlayingMicros() const {
#ifdef SUPERSONIC_LINK
    return mImpl->link.captureAppSessionState().timeForIsPlaying().count();
#else
    return 0;
#endif
}

double SuperClock::beatAtLinkTime(int64_t timeMicros, double quantum) const {
#ifdef SUPERSONIC_LINK
    return mImpl->link.captureAppSessionState().beatAtTime(
        std::chrono::microseconds{timeMicros}, quantum);
#else
    (void)timeMicros; (void)quantum;
    return 0.0;
#endif
}

double SuperClock::phaseAtLinkTime(int64_t timeMicros, double quantum) const {
#ifdef SUPERSONIC_LINK
    return mImpl->link.captureAppSessionState().phaseAtTime(
        std::chrono::microseconds{timeMicros}, quantum);
#else
    (void)timeMicros; (void)quantum;
    return 0.0;
#endif
}

int64_t SuperClock::timeAtBeatLinkMicros(double beat, double quantum) const {
#ifdef SUPERSONIC_LINK
    return mImpl->link.captureAppSessionState()
        .timeAtBeat(beat, quantum).count();
#else
    (void)beat; (void)quantum;
    return 0;
#endif
}

// Link↔NTP domain mapping. Native+Link: 0/0 fallback never used (Link
// RPC methods above don't compose via these). Provided for header
// completeness in case future shared code wants to convert.
double SuperClock::linkMicrosToNtpSeconds(int64_t linkMicros) const {
    return static_cast<double>(linkMicros) / 1'000'000.0;
}
int64_t SuperClock::ntpSecondsToLinkMicros(double ntpSeconds) const {
    return static_cast<int64_t>(ntpSeconds * 1'000'000.0);
}

// ─── Link event callbacks ───────────────────────────────────────────────

void SuperClock::setTempoChangedCallback(std::function<void(double)> cb) {
#ifdef SUPERSONIC_LINK
    // Mirror remote-peer tempo into the SAB and recompute
    // beat_origin_ntp under the new tempo so mirror-domain beatAtTime
    // stays coherent with Link-domain beatAtTime.
    auto* impl = mImpl.get();
    mImpl->link.setTempoCallback(
        [cb = std::move(cb), impl](const double bpmIn) {
            double bpm = bpmIn;
            if (!(bpm >= 1.0)) bpm = 1.0;
            impl->boundState->bpm.store(doubleToBits(bpm),
                                        std::memory_order_relaxed);
            auto st = impl->link.captureAppSessionState();
            const auto now = impl->link.clock().micros();
            constexpr double kBeatOriginQuantum = 4.0;
            const double currentBeat = st.beatAtTime(now, kBeatOriginQuantum);
            const double wallNow = wallClockNTP();
            const double newOrigin = wallNow - currentBeat * 60.0 / bpm;
            impl->boundState->beat_origin_ntp.store(
                doubleToBits(newOrigin), std::memory_order_relaxed);
            // cb may be empty (shutdown's setTempoChangedCallback({})).
            // Pass clamped bpm so a corrupt peer's NaN doesn't leak out.
            if (cb) cb(bpm);
        });
#else
    (void)cb;
#endif
}

void SuperClock::setNumPeersChangedCallback(std::function<void(std::size_t)> cb) {
#ifdef SUPERSONIC_LINK
    mImpl->link.setNumPeersCallback(
        [cb = std::move(cb)](const std::size_t n) { if (cb) cb(n); });
#else
    (void)cb;
#endif
}

void SuperClock::setStartStopChangedCallback(
    std::function<void(bool, int64_t)> cb) {
#ifdef SUPERSONIC_LINK
    auto* impl = mImpl.get();
    mImpl->link.setStartStopCallback(
        [cb = std::move(cb), impl](const bool playing) {
            impl->boundState->is_playing.store(playing ? 1u : 0u,
                                               std::memory_order_relaxed);
            // Mirror is_playing_at_ntp too so consumers reading both
            // (e.g. /superclock_get → beats-since-transport-start math)
            // don't see new is_playing paired with stale timestamp.
            // wallClockNTP() is the closest NTP-domain timestamp to
            // "right now" we have on the Link network thread.
            impl->boundState->is_playing_at_ntp.store(
                doubleToBits(wallClockNTP()), std::memory_order_relaxed);
            const auto t = impl->link.captureAppSessionState()
                                     .timeForIsPlaying().count();
            if (cb) cb(playing, t);
        });
#else
    (void)cb;
#endif
}

// ─── Link Audio: visibility / publish / peer name ───────────────────────

void SuperClock::setLinkVisibility(LinkVisibility v) {
#ifdef SUPERSONIC_LINK
    if (v == getLinkVisibility()) return;

    if (v != LinkVisibility::Off) mImpl->lastNonOffVisibility = v;

    // Tear down then bring up. Disabling Link forces immediate gateway
    // teardown so the loopback flag transition is instant — no ~5 s
    // rescan window. Input subs / aux sinks / main sink bind to the
    // current Link substrate (session-scoped channelIds), so clear
    // them BEFORE enableLinkAudio(false) — app must re-add after the
    // next non-Off transition.
    {
        std::lock_guard<std::mutex> lk(mImpl->inputSubMutex);
        mImpl->inputSubs.clear();
        mImpl->inputSubCount.store(0, std::memory_order_relaxed);
    }
    {
        std::lock_guard<std::mutex> auxLk(mImpl->auxSinksMutex);
        mImpl->auxSinks.clear();
        mImpl->auxSinkCount.store(0, std::memory_order_relaxed);
    }
    {
        std::lock_guard<std::mutex> lk(mImpl->sinkMutex);
        if (mImpl->sink) mImpl->sink.reset();
    }
    mImpl->link.enableLinkAudio(false);
    mImpl->link.enable(false);
    auto* impl = mImpl.get();
    mImpl->link.callOnLinkThread([impl]() { impl->threadPriority.reset(); });

    // Interface-filter flag picked up by the next enable().
    linkLoopbackOnlyFlag().store(
        v == LinkVisibility::LoopbackOnly, std::memory_order_relaxed);

    if (v == LinkVisibility::Off) {
        state()->flags.fetch_and(~SC_FLAG_LINK_ENABLED, std::memory_order_relaxed);
        return;
    }

    // Flag-flip before link.enable(true) so observers never see
    // "Link on, flag off" mid-transition.
    state()->flags.fetch_or(SC_FLAG_LINK_ENABLED, std::memory_order_relaxed);

    // Bring up in order: enable (ScanIpIfAddrs respects loopback flag),
    // raise thread priority, enableLinkAudio unconditionally (so we
    // can observe peer channels even in receive-only mode), then sink
    // only if publishing was requested.
    mImpl->link.enable(true);
    mImpl->link.callOnLinkThread([impl]() { impl->threadPriority.setHigh(); });
    mImpl->link.enableLinkAudio(true);
    if (mImpl->audioPublishEnabled) {
        std::lock_guard<std::mutex> lk(mImpl->sinkMutex);
        mImpl->sink.emplace(mImpl->link, mImpl->channelNameCache, Impl::kSinkMaxSamples);
    }
#else
    (void)v;
#endif
}

SuperClock::LinkVisibility SuperClock::getLinkVisibility() const {
#ifdef SUPERSONIC_LINK
    if (!mImpl->link.isEnabled()) return LinkVisibility::Off;
    return linkLoopbackOnlyFlag().load(std::memory_order_relaxed)
        ? LinkVisibility::LoopbackOnly
        : LinkVisibility::NetworkWide;
#else
    return LinkVisibility::Off;
#endif
}

void SuperClock::setLinkAudioPublish(bool publish) {
#ifdef SUPERSONIC_LINK
    if (mImpl->audioPublishEnabled == publish) return;
    mImpl->audioPublishEnabled = publish;
    if (publish) state()->flags.fetch_or(SC_FLAG_LINK_AUDIO_PUBLISH,  std::memory_order_relaxed);
    else         state()->flags.fetch_and(~SC_FLAG_LINK_AUDIO_PUBLISH, std::memory_order_relaxed);

    // Reflect immediately if the Link mesh is already up; otherwise the
    // next setLinkVisibility(non-Off) creates the sink. LinkAudio
    // stays enabled in both directions — we keep observing peers'
    // channels even after we stop publishing.
    if (!mImpl->link.isEnabled()) return;
    if (publish) {
        std::lock_guard<std::mutex> lk(mImpl->sinkMutex);
        mImpl->sink.emplace(mImpl->link, mImpl->channelNameCache, Impl::kSinkMaxSamples);
    } else {
        // Aux sinks share the publish-side substrate — clear them when
        // publish goes off; the main sink stops broadcasting too.
        {
            std::lock_guard<std::mutex> auxLk(mImpl->auxSinksMutex);
            mImpl->auxSinks.clear();
            mImpl->auxSinkCount.store(0, std::memory_order_relaxed);
        }
        {
            std::lock_guard<std::mutex> lk(mImpl->sinkMutex);
            if (mImpl->sink) mImpl->sink.reset();
        }
    }
#else
    (void)publish;
#endif
}

bool SuperClock::isLinkAudioPublishEnabled() const {
#ifdef SUPERSONIC_LINK
    return mImpl->audioPublishEnabled;
#else
    return false;
#endif
}

void SuperClock::setPeerName(const char* name) {
#ifdef SUPERSONIC_LINK
    std::string requested = name ? name : "";
    if (mImpl->peerNameCache == requested) return;
    mImpl->peerNameCache = std::move(requested);
    mImpl->link.setPeerName(mImpl->peerNameCache);
#else
    (void)name;
#endif
}

const char* SuperClock::peerName() const {
#ifdef SUPERSONIC_LINK
    return mImpl->peerNameCache.c_str();
#else
    return "";
#endif
}

// ─── Link Audio: peer + channel discovery ───────────────────────────────

std::vector<SuperClock::PeerInfo> SuperClock::listPeers() const {
    std::vector<PeerInfo> out;
#ifdef SUPERSONIC_LINK
    if (!mImpl->link.isEnabled()) return out;
    auto peers = mImpl->link.sessionPeers();
    out.reserve(peers.size());
    for (auto& p : peers) {
        const auto& st = p.first;
        const auto& gw = p.second;
        PeerInfo info;
        info.nodeId          = bytesToHex16(st.ident());
        info.gatewayIp       = gw.to_string();
        info.isLoopback      = gw.is_loopback();
        info.measurementIp   = st.measurementEndpoint.address().to_string();
        info.measurementPort = st.measurementEndpoint.port();
        if (st.audioEndpoint) {
            info.audioIp   = st.audioEndpoint->address().to_string();
            info.audioPort = st.audioEndpoint->port();
        }
        out.push_back(std::move(info));
    }
#endif
    return out;
}

std::vector<SuperClock::LinkAudioChannel>
SuperClock::listLinkAudioChannels() const {
    std::vector<LinkAudioChannel> out;
#ifdef SUPERSONIC_LINK
    auto channels = mImpl->link.channels();
    out.reserve(channels.size());
    for (auto& c : channels) {
        out.push_back({bytesToHex16(c.id), c.name,
                       bytesToHex16(c.peerId), c.peerName});
    }
#endif
    return out;
}

// ─── Link Audio: input subscription ─────────────────────────────────────

bool SuperClock::addLinkAudioInput(const char* peerName,
                                    const char* channelName,
                                    uint32_t busIdx) {
#ifdef SUPERSONIC_LINK
    if (!peerName || !channelName) return false;

    auto channels = mImpl->link.channels();
    auto match = std::find_if(channels.begin(), channels.end(),
        [&](const auto& c) {
            return c.peerName == peerName && c.name == channelName;
        });
    if (match == channels.end()) return false;
    const ableton::ChannelId newChannelId = match->id;

    // First pass: re-arm short-circuit (matching channelId reuses
    // the existing renderer + counters). Subscribe stays outside the
    // lock — Link's source-callback interacts with its own threading.
    {
        std::lock_guard<std::mutex> lk(mImpl->inputSubMutex);
        Impl::InputSubscription* existing = nullptr;
        for (auto& s : mImpl->inputSubs) {
            if (s.peerName == peerName && s.channelName == channelName) {
                existing = &s;
                continue;
            }
            const uint32_t sLo = s.busIdx;
            const uint32_t sHi = s.busIdx + 1;
            const uint32_t nLo = busIdx;
            const uint32_t nHi = busIdx + 1;
            if (sLo <= nHi && nLo <= sHi) return false;
        }
        if (existing && existing->channelId == newChannelId) {
            existing->busIdx = busIdx;
            return true;
        }
    }
    // Build a fresh renderer outside the lock.
    Impl::InputSubscription sub;
    sub.busIdx      = busIdx;
    sub.peerName    = peerName;
    sub.channelName = channelName;
    sub.channelId   = newChannelId;
    sub.renderer = std::make_unique<
        supersonic_link::LinkAudioInputRenderer<ableton::LinkAudio>>(mImpl->link);
    sub.renderer->subscribe(newChannelId);

    std::lock_guard<std::mutex> lk(mImpl->inputSubMutex);
    // Re-validate under the lock; state may have shifted while we
    // were building.
    Impl::InputSubscription* replaceSlot = nullptr;
    for (auto& s : mImpl->inputSubs) {
        if (s.peerName == peerName && s.channelName == channelName) {
            replaceSlot = &s;
            continue;
        }
        const uint32_t sLo = s.busIdx;
        const uint32_t sHi = s.busIdx + 1;
        const uint32_t nLo = busIdx;
        const uint32_t nHi = busIdx + 1;
        if (sLo <= nHi && nLo <= sHi) return false;
    }
    if (replaceSlot) {
        *replaceSlot = std::move(sub);
        return true;
    }
    mImpl->inputSubs.push_back(std::move(sub));
    mImpl->inputSubCount.store(mImpl->inputSubs.size(),
                                std::memory_order_relaxed);
    return true;
#else
    (void)peerName; (void)channelName; (void)busIdx;
    return false;
#endif
}

void SuperClock::removeLinkAudioInput(const char* peerName,
                                       const char* channelName) {
#ifdef SUPERSONIC_LINK
    if (!peerName || !channelName) return;
    std::lock_guard<std::mutex> lk(mImpl->inputSubMutex);
    auto& v = mImpl->inputSubs;
    v.erase(std::remove_if(v.begin(), v.end(),
            [&](const auto& s) {
                return s.peerName == peerName && s.channelName == channelName;
            }),
            v.end());
    mImpl->inputSubCount.store(v.size(), std::memory_order_relaxed);
#else
    (void)peerName; (void)channelName;
#endif
}

void SuperClock::clearLinkAudioInputs() {
#ifdef SUPERSONIC_LINK
    std::lock_guard<std::mutex> lk(mImpl->inputSubMutex);
    mImpl->inputSubs.clear();
    mImpl->inputSubCount.store(0, std::memory_order_relaxed);
#endif
}

bool SuperClock::setLinkAudioInputLatencySeconds(const char* peerName,
                                                  const char* channelName,
                                                  double seconds) {
#ifdef SUPERSONIC_LINK
    if (!peerName || !channelName) return false;
    if (!(seconds >= 0.0) ||
        seconds > kMaxLinkAudioInputLatencySeconds) {
        return false;
    }
    std::lock_guard<std::mutex> lk(mImpl->inputSubMutex);
    for (auto& sub : mImpl->inputSubs) {
        if (sub.peerName == peerName && sub.channelName == channelName) {
            sub.renderer->setLatencySeconds(seconds);
            return true;
        }
    }
    return false;
#else
    (void)peerName; (void)channelName; (void)seconds;
    return false;
#endif
}

std::vector<SuperClock::LinkAudioInputStatus>
SuperClock::listLinkAudioInputs() const {
    std::vector<LinkAudioInputStatus> out;
#ifdef SUPERSONIC_LINK
    std::lock_guard<std::mutex> lk(mImpl->inputSubMutex);
    out.reserve(mImpl->inputSubs.size());
    for (const auto& sub : mImpl->inputSubs) {
        LinkAudioInputStatus s;
        s.peerName          = sub.peerName;
        s.channelName       = sub.channelName;
        s.busIdx            = sub.busIdx;
        s.sampleRate        = sub.renderer->lastSampleRate();
        s.sourceNumChannels = sub.renderer->lastNumChannels();
        s.bufferedSeconds      = sub.renderer->bufferedSeconds();
        s.droppedSourceBuffers    = sub.renderer->droppedSourceBuffers();
        s.networkGapBuffers       = sub.renderer->networkGapBuffers();
        s.totalSourceBufferCalls  = sub.renderer->totalSourceBufferCalls();
        s.duplicateCountCalls     = sub.renderer->duplicateCountCalls();
        s.latencySeconds          = sub.renderer->latencySeconds();

        constexpr float kMinHealthyBufferSeconds = 0.005f;
        const bool everReceived = sub.renderer->everReceived();
        if (!everReceived) {
            s.state = LinkAudioConnectionState::Connecting;
        } else if (s.bufferedSeconds < kMinHealthyBufferSeconds) {
            s.state = LinkAudioConnectionState::Dropout;
        } else {
            s.state = LinkAudioConnectionState::Connected;
        }
        out.push_back(std::move(s));
    }
#endif
    return out;
}

// ─── Link Audio: auxiliary sinks ────────────────────────────────────────

bool SuperClock::addLinkAudioSink(const char* name,
                                   uint32_t busIdx,
                                   uint32_t numChannels) {
#ifdef SUPERSONIC_LINK
    if (!name || numChannels == 0 || numChannels > 2) return false;
    // Construct outside the lock — LinkAudioSink ctor isn't RT-safe but
    // we're on the app thread, and we don't want it inside the lock.
    Impl::ActiveSink entry{
        std::string(name),
        busIdx,
        numChannels,
        ableton::LinkAudioSink(mImpl->link, std::string(name),
                                Impl::kSinkMaxSamples)
    };
    std::lock_guard<std::mutex> lk(mImpl->auxSinksMutex);
    for (auto& as : mImpl->auxSinks) {
        if (as.name == name) {
            as = std::move(entry);
            return true;
        }
    }
    mImpl->auxSinks.push_back(std::move(entry));
    mImpl->auxSinkCount.store(mImpl->auxSinks.size(),
                              std::memory_order_relaxed);
    return true;
#else
    (void)name; (void)busIdx; (void)numChannels;
    return false;
#endif
}

void SuperClock::removeLinkAudioSink(const char* name) {
#ifdef SUPERSONIC_LINK
    if (!name) return;
    std::lock_guard<std::mutex> lk(mImpl->auxSinksMutex);
    auto& v = mImpl->auxSinks;
    v.erase(std::remove_if(v.begin(), v.end(),
            [&](const auto& as) { return as.name == name; }),
            v.end());
    mImpl->auxSinkCount.store(v.size(), std::memory_order_relaxed);
#else
    (void)name;
#endif
}

std::vector<SuperClock::ActiveSinkInfo> SuperClock::listLinkAudioSinks() const {
    std::vector<ActiveSinkInfo> out;
#ifdef SUPERSONIC_LINK
    std::lock_guard<std::mutex> lk(mImpl->auxSinksMutex);
    out.reserve(mImpl->auxSinks.size());
    for (const auto& as : mImpl->auxSinks) {
        out.push_back({as.name, as.busIdx, as.numChannels, as.hasSubscriber});
    }
#endif
    return out;
}

// ─── Audio-thread: publish + drain (RT-safe) ────────────────────────────

void SuperClock::publishAuxSinks(const float* busPool,
                                  uint32_t blockSize,
                                  uint32_t numBuses,
                                  uint32_t sampleRate,
                                  uint64_t hostMicrosForBufferBegin,
                                  double quantum) {
#ifdef SUPERSONIC_LINK
    if (!busPool) return;
    // Fast path: skip the mutex entirely when there are no aux sinks.
    if (mImpl->auxSinkCount.load(std::memory_order_relaxed) == 0) return;
    // try_lock keeps the audio thread RT-friendly: skip block on
    // contention, next one recovers.
    std::unique_lock<std::mutex> lk(mImpl->auxSinksMutex, std::try_to_lock);
    if (!lk.owns_lock()) return;
    if (mImpl->auxSinks.empty()) return;

    auto sessionState = mImpl->link.captureAudioSessionState();
    const auto hostMicros = hostMicrosOrNow(hostMicrosForBufferBegin, mImpl->link);
    const double beatsAtBegin = sessionState.beatAtTime(hostMicros, quantum);

    for (auto& as : mImpl->auxSinks) {
        if (as.busIdx + as.numChannels > numBuses) continue;
        ableton::LinkAudioSink::BufferHandle buf(as.sink);
        const bool subscribed = static_cast<bool>(buf);
        if (subscribed != as.hasSubscriber) as.hasSubscriber = subscribed;
        if (!subscribed) continue;
        if (blockSize * as.numChannels > buf.maxNumSamples) continue;

        const float* L = busPool + as.busIdx * blockSize;
        const float* R = as.numChannels == 2
            ? busPool + (as.busIdx + 1) * blockSize : nullptr;
        interleaveFloatToInt16(L, R, buf.samples, blockSize);
        buf.commit(sessionState, beatsAtBegin, quantum,
                    blockSize, as.numChannels, sampleRate);
    }
#else
    (void)busPool; (void)blockSize; (void)numBuses;
    (void)sampleRate; (void)hostMicrosForBufferBegin; (void)quantum;
#endif
}

void SuperClock::drainLinkAudioInputsToBuses(float* busPool,
                                              uint32_t blockSize,
                                              uint32_t numBuses,
                                              uint32_t sampleRate,
                                              uint64_t hostMicrosForBufferBegin) {
#ifdef SUPERSONIC_LINK
    if (!busPool) return;
    // Lock-free fast path: no subscriptions, no mutex acquisition.
    if (mImpl->inputSubCount.load(std::memory_order_relaxed) == 0) return;
    // try_lock: skip the block if an OSC-driven add/remove/clear is
    // mid-mutation. Next block recovers.
    std::unique_lock<std::mutex> lk(mImpl->inputSubMutex, std::try_to_lock);
    if (!lk.owns_lock()) return;
    if (mImpl->inputSubs.empty()) return;

    // Each subscription renders stereo into (busIdx, busIdx+1).
    // Mono sources are mirrored to both buses by the renderer.
    double* const scratchL = mImpl->drainScratchL;
    double* const scratchR = mImpl->drainScratchR;
    // Skip the whole drain if blockSize exceeds the scratch — partial
    // fills would leave the bus's tail silent while touch_audio_bus
    // marks it fresh, which In.ar consumers can't distinguish from
    // real audio.
    if (blockSize > Impl::kDrainScratchFrames) return;
    const size_t framesToRender = blockSize;

    auto sessionState = mImpl->link.captureAudioSessionState();
    const auto hostTime = hostMicrosOrNow(hostMicrosForBufferBegin, mImpl->link);

    for (auto& sub : mImpl->inputSubs) {
        if (sub.busIdx + 1 >= numBuses) continue;
        const size_t framesFilled = sub.renderer->receive(
            scratchL, scratchR, framesToRender, sessionState,
            static_cast<double>(sampleRate), hostTime, /*quantum=*/4.0);

        // Couldn't fill the whole block → the queue ran dry: an underrun.
        if (framesFilled < framesToRender)
            mImpl->linkAudioUnderruns.fetch_add(1, std::memory_order_relaxed);

        float* dstL = busPool +  sub.busIdx      * blockSize;
        float* dstR = busPool + (sub.busIdx + 1) * blockSize;
        for (size_t i = 0; i < framesFilled; ++i) {
            dstL[i] = static_cast<float>(scratchL[i]);
            dstR[i] = static_cast<float>(scratchR[i]);
        }
        for (size_t i = framesFilled; i < blockSize; ++i) {
            dstL[i] = 0.0f;
            dstR[i] = 0.0f;
        }
        touch_audio_bus_for_next_block(sub.busIdx);
        touch_audio_bus_for_next_block(sub.busIdx + 1);
    }
#else
    (void)busPool; (void)blockSize; (void)numBuses;
    (void)sampleRate; (void)hostMicrosForBufferBegin;
#endif
}

bool SuperClock::publishAudioBlock(const float* leftChannel,
                                    const float* rightChannel,
                                    size_t numFrames,
                                    uint32_t sampleRate,
                                    uint64_t hostMicrosForBufferBegin,
                                    double quantum) {
#ifdef SUPERSONIC_LINK
    // try_lock: skip the publish if setLinkVisibility /
    // setLinkAudioPublish is mid-reset of mImpl->sink.
    std::unique_lock<std::mutex> lk(mImpl->sinkMutex, std::try_to_lock);
    if (!lk.owns_lock()) return false;
    if (!mImpl->sink) return false;
    ableton::LinkAudioSink::BufferHandle buf(*mImpl->sink);
    if (!buf) return false;

    const size_t numChannels = rightChannel ? 2u : 1u;
    if (numFrames * numChannels > buf.maxNumSamples) return false;

    interleaveFloatToInt16(leftChannel, rightChannel, buf.samples, numFrames);

    const auto hostMicros = hostMicrosOrNow(hostMicrosForBufferBegin, mImpl->link);
    auto st = mImpl->link.captureAudioSessionState();
    const double beatsAtBegin = st.beatAtTime(hostMicros, quantum);

    return buf.commit(st, beatsAtBegin, quantum,
                       numFrames, numChannels, sampleRate);
#else
    (void)leftChannel; (void)rightChannel; (void)numFrames;
    (void)sampleRate; (void)hostMicrosForBufferBegin; (void)quantum;
    return false;
#endif
}

void SuperClock::publishLinkMetrics(PerformanceMetrics* m, double quantum) {
#ifdef SUPERSONIC_LINK
    if (!m) return;

    // One realtime-safe, lock-free capture, read coherently for every Link
    // field below. This MUST use captureAudioSessionState() (the audio-thread
    // variant): the convenience getters tempo()/isPlaying() and the NTP-domain
    // beatAtTime()/phaseAtTime() each reach for captureAppSessionState(), which
    // takes a lock and must never run on the audio thread. Beat/phase are
    // queried in Link's own clock domain at "now", mirroring
    // drainLinkAudioInputsToBuses / the tempo-changed callback.
    const auto st = mImpl->link.captureAudioSessionState();
    const auto nowMicros = mImpl->link.clock().micros();

    // Clock readouts — always written so the LINK card is live even with no
    // Link Audio subscriptions.
    m->link_peers.store(static_cast<uint32_t>(mImpl->link.numPeers()),
                        std::memory_order_relaxed);
    const double bpm = st.tempo();
    m->link_tempo_mbpm.store(bpm > 0.0 ? static_cast<uint32_t>(bpm * 1000.0 + 0.5) : 0u,
                             std::memory_order_relaxed);
    const double beat = st.beatAtTime(nowMicros, quantum);
    m->link_beat_centi.store(beat > 0.0 ? static_cast<uint32_t>(beat * 100.0) : 0u,
                             std::memory_order_relaxed);
    const double phase = st.phaseAtTime(nowMicros, quantum);
    m->link_phase_centi.store(phase > 0.0 ? static_cast<uint32_t>(phase * 100.0) : 0u,
                              std::memory_order_relaxed);
    m->link_playing.store(st.isPlaying() ? 1u : 0u, std::memory_order_relaxed);

    // Link Audio — output.
    m->link_audio_publish.store(mImpl->audioPublishEnabled ? 1u : 0u,
                                std::memory_order_relaxed);
    {
        std::unique_lock<std::mutex> lk(mImpl->auxSinksMutex, std::try_to_lock);
        if (lk.owns_lock())
            m->link_audio_sinks.store(static_cast<uint32_t>(mImpl->auxSinks.size()),
                                      std::memory_order_relaxed);
    }

    // Link Audio — receive health (aggregated across input subscriptions).
    m->link_audio_underruns.store(mImpl->linkAudioUnderruns.load(std::memory_order_relaxed),
                                  std::memory_order_relaxed);
    {
        std::unique_lock<std::mutex> lk(mImpl->inputSubMutex, std::try_to_lock);
        if (lk.owns_lock()) {
            uint32_t inCh = 0, rate = 0;
            int32_t  drift = 0;
            float    bufMs = 0.0f;
            for (auto& sub : mImpl->inputSubs) {
                inCh += sub.renderer->lastNumChannels();
                rate  = sub.renderer->lastSampleRate();
                drift = sub.renderer->lastDriftPpm();
                bufMs = std::max(bufMs, sub.renderer->bufferedSeconds() * 1000.0f);
            }
            m->link_audio_in_channels.store(inCh, std::memory_order_relaxed);
            m->link_audio_stream_rate.store(rate, std::memory_order_relaxed);
            m->link_audio_drift_ppm.store(drift, std::memory_order_relaxed);
            m->link_audio_buffered_ms.store(static_cast<uint32_t>(bufMs),
                                            std::memory_order_relaxed);
        }
    }
#else
    (void)m; (void)ntpSeconds; (void)quantum;
#endif
}

// ─── Audio-thread NTP (IIR-smoothed wall clock) ─────────────────────────

double SuperClock::now() const {
    const uint64_t bits =
        mImpl->currentAudioThreadNTPBits.load(std::memory_order_acquire);
    if (bits == 0) return wallClockNTP();
    return bitsToDouble(bits);
}

double SuperClock::nowAt(double audioCurrentTime) const {
    (void)audioCurrentTime;  // Native uses its own time source.
    return now();
}

double SuperClock::wallNow() const {
    return wallClockNTP();
}

double SuperClock::updateAudioThreadNTP(double samplePosition,
                                         double sampleRate,
                                         double audioCurrentTime) {
    (void)audioCurrentTime;
    const double sampleOffsetSec = samplePosition / sampleRate;

    // Freewheel: pure sample-derived NTP, no wall-clock drift IIR. The
    // headless driver thread can be preempted on a busy machine; chasing that
    // as "drift" injects scheduling jitter a real device callback never sees.
    // Deterministic for offline/accuracy tests.
    if (mImpl->freewheelClock.load(std::memory_order_relaxed)) {
        const double wallNTP =
            bitsToDouble(mImpl->baseNTPBits.load(std::memory_order_relaxed))
            + sampleOffsetSec;
        mImpl->currentAudioThreadNTPBits.store(doubleToBits(wallNTP),
                                                std::memory_order_release);
        return wallNTP;
    }

    //   sampleNTP = mBaseNTP + samplePosition / sampleRate
    //   drift     = wallNow - sampleNTP
    //   mBaseNTP += drift * 0.01   (low-pass converge ~1% per call)
    //   result    = mBaseNTP + samplePosition / sampleRate
    const double wallNow = wallClockNTP();
    const double baseNTP = bitsToDouble(
        mImpl->baseNTPBits.load(std::memory_order_relaxed));
    const double drift = wallNow - (baseNTP + sampleOffsetSec);
    const double newBaseNTP = baseNTP + drift * 0.01;
    const double wallNTP = newBaseNTP + sampleOffsetSec;
    mImpl->baseNTPBits.store(doubleToBits(newBaseNTP), std::memory_order_relaxed);
    mImpl->currentAudioThreadNTPBits.store(doubleToBits(wallNTP),
                                            std::memory_order_release);
    return wallNTP;
}

void SuperClock::resetAudioThreadTime(double samplePosition, double sampleRate) {
    const double sampleOffsetSec = samplePosition / sampleRate;
    const double newBaseNTP = wallClockNTP() - sampleOffsetSec;
    mImpl->baseNTPBits.store(doubleToBits(newBaseNTP), std::memory_order_relaxed);
    mImpl->currentAudioThreadNTPBits.store(
        doubleToBits(newBaseNTP + sampleOffsetSec),
        std::memory_order_release);
    // Re-anchor the Link-domain audio host clock on the next call.
    mImpl->linkAudioHostAnchored = false;
}

// Drift-corrected host time (Link clock domain, µs) for the Link Audio receive
// resampler: derived from the monotonic sample counter (jitter-free) and
// slow-IIR-corrected toward link.clock().micros(). Reading link.clock() at
// audio-thread wake is jittery; the IIR rejects that while tracking real drift.
int64_t SuperClock::linkAudioHostMicros(double samplePosition, double sampleRate) {
#ifdef SUPERSONIC_LINK
    if (sampleRate <= 0.0) return mImpl->link.clock().micros().count();
    const double sampleOffsetMicros = (samplePosition / sampleRate) * 1e6;
    const double linkNow = static_cast<double>(mImpl->link.clock().micros().count());

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
#else
    (void)samplePosition; (void)sampleRate;
    return 0;
#endif
}

void SuperClock::setFreewheelClock(bool enabled) {
    mImpl->freewheelClock.store(enabled, std::memory_order_relaxed);
}
