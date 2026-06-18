/*
 * LinkSession.cpp — Ableton Link clock-sync session (real impl).
 *
 * Compiled only when SUPERSONIC_LINK. Owns the ableton::LinkAudio instance, the
 * network-thread priority, the advertised peer name, the loopback-only interface
 * filter, and the deferred enable/disable worker thread. Session mutators and
 * Link's tempo/transport callbacks mirror the converged values into the
 * SuperClockState SAB region owned by the borrowed SuperClock&. The Link Audio
 * bus bridge borrows the instance via linkAudio(); visibility orchestration
 * (sink/sub teardown ordering) is composed by SuperClock::setLinkVisibility.
 */
#include "native/LinkSession.h"

#if SUPERSONIC_LINK

#include "native/WallClock.h"
#include "shared_memory.h"

// LinkAudio.hpp must be used INSTEAD of Link.hpp ("LinkAudio and Link should not
// be used simultaneously" per Ableton's docs).
#include <ableton/LinkAudio.hpp>

// Loopback-only interface filtering. The flag is the one our
// link-loopback-mode.patch adds to the platform's ScanIpIfAddrs.hpp;
// setLoopbackOnly toggles it and Link's InterfaceScanner constrains
// discovery/multicast/unicast to loopback on its next scan. Same flag on both
// platforms — keep the include and the accessor switched in lockstep.
#if defined(_WIN32)
#include <ableton/platforms/windows/ScanIpIfAddrs.hpp>
#else
#include <ableton/platforms/posix/ScanIpIfAddrs.hpp>
#endif

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>

using supersonic::doubleToBits;

namespace {

inline std::atomic<bool>& linkLoopbackOnlyFlag() {
#if defined(_WIN32)
    return ableton::platforms::windows::loopbackOnly();
#else
    return ableton::platforms::posix::loopbackOnly();
#endif
}

// Render an 8-byte Link NodeId/PeerId/ChannelId as 16 lowercase hex.
template <typename Bytes>
inline std::string bytesToHex16(const Bytes& bytes) {
    char buf[17];
    for (size_t k = 0; k < bytes.size(); ++k) {
        std::snprintf(buf + 2 * k, 3, "%02x", bytes[k]);
    }
    return std::string(buf, 16);
}

}  // namespace

// ─── Impl ────────────────────────────────────────────────────────────────────

struct LinkSession::Impl {
    SuperClock& clock;

    // Off RT thread, ~every 250 ms — drives MIDI follower staleness. Wired by
    // SuperClock so the session never references MIDI.
    std::function<void()> periodicTick;

    // The deferred worker resolves an async enable/disable into a visibility
    // transition through SuperClock::setLinkVisibility (which orders Link Audio
    // sink/sub teardown). Set after construction.
    std::function<void(LinkVisibility)> applyVisibility;

    // Link starts disabled; setLinkVisibility(non-Off) flips peer discovery on.
    // While disabled the instance still hosts session state — we read
    // tempo/transport from it even with no peers.
    ableton::LinkAudio link{120.0, "SuperSonic"};

    // RT priority for Link's network thread. Without it the receiver hears
    // periodic packet-burst pulsing when the OS preempts the thread (mirrors
    // Ableton's linkaudiohut example).
    ableton::link::platform::ThreadPriority threadPriority{};

    // Backing storage for the peer name we advertise. Link::setName takes by
    // value; we keep the C-string stable for peerName().
    std::string peerNameCache{"SuperSonic"};

    // Last non-Off visibility, restored by setLinkEnabled(false→true). Defaults
    // to LoopbackOnly: bare enable on a fresh SuperClock stays loopback-only
    // until the caller explicitly opts into NetworkWide via setLinkVisibility.
    LinkVisibility lastNonOffVisibility{LinkVisibility::LoopbackOnly};

    // Deferred Link enable/disable for audio-thread callers.
    // -1 = no request, 0 = disable, 1 = enable.
    std::thread             deferredWorker;
    std::mutex              deferredMtx;
    std::condition_variable deferredCv;
    std::atomic<int>        deferredLinkEnableReq{-1};
    std::atomic<bool>       deferredQuit{false};

    explicit Impl(SuperClock& c, std::function<void()> tick)
        : clock(c), periodicTick(std::move(tick)) {}
};

// ─── ctor/dtor ─────────────────────────────────────────────────────────────

LinkSession::LinkSession(SuperClock& clock, std::function<void()> periodicTick)
    : mImpl(std::make_unique<Impl>(clock, std::move(periodicTick))) {
    auto* impl = mImpl.get();
    impl->deferredWorker = std::thread([impl] {
        for (;;) {
            {
                std::unique_lock<std::mutex> lk(impl->deferredMtx);
                // Wake on a request/quit, or every ~250 ms to sweep midi
                // timeline staleness (freeze stale tempo, free after grace).
                impl->deferredCv.wait_for(lk, std::chrono::milliseconds(250), [impl] {
                    return impl->deferredQuit.load(std::memory_order_acquire)
                        || impl->deferredLinkEnableReq.load(
                               std::memory_order_acquire) != -1;
                });
                if (impl->deferredQuit.load(std::memory_order_acquire)) return;
                const int req = impl->deferredLinkEnableReq.exchange(
                    -1, std::memory_order_acq_rel);
                lk.unlock();
                // Resolve enable/disable into a visibility transition, matching
                // SuperClock::setLinkEnabled: enable from Off restores the last
                // non-Off visibility; disable goes to Off; enable while already
                // enabled is a no-op (applyVisibility short-circuits same-state).
                if (req == 1) {
                    if (impl->applyVisibility &&
                        !impl->link.isEnabled()) {
                        impl->applyVisibility(impl->lastNonOffVisibility);
                    }
                } else if (req == 0) {
                    if (impl->applyVisibility) impl->applyVisibility(LinkVisibility::Off);
                }
            }
            if (impl->periodicTick) impl->periodicTick();
        }
    });
}

LinkSession::~LinkSession() {
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
}

// ─── Session mutators (mirror into the SAB) ──────────────────────────────────

void LinkSession::setBpm(double bpm) {
    auto st = mImpl->link.captureAppSessionState();
    st.setTempo(bpm, mImpl->link.clock().micros());
    mImpl->link.commitAppSessionState(st);
    // Mirror the input. Link's tempo callback re-syncs the mirror to Link's
    // converged value when peers exist.
    SuperClockState* s = mImpl->clock.state();
    if (!s) return;
    s->bpm.store(doubleToBits(bpm), std::memory_order_relaxed);
}

void LinkSession::setIsPlaying(bool playing, double atNtpSeconds) {
    auto st = mImpl->link.captureAppSessionState();
    st.setIsPlaying(playing, mImpl->link.clock().micros());
    mImpl->link.commitAppSessionState(st);
    SuperClockState* s = mImpl->clock.state();
    if (!s) return;
    s->is_playing_at_ntp.store(doubleToBits(atNtpSeconds), std::memory_order_relaxed);
    s->is_playing.store(playing ? 1u : 0u, std::memory_order_relaxed);
}

void LinkSession::setStartStopSyncEnabled(bool enabled) {
    mImpl->link.enableStartStopSync(enabled);
    SuperClockState* s = mImpl->clock.state();
    if (!s) return;
    if (enabled) s->flags.fetch_or(SC_FLAG_START_STOP_SYNC,  std::memory_order_relaxed);
    else         s->flags.fetch_and(~SC_FLAG_START_STOP_SYNC, std::memory_order_relaxed);
}

void LinkSession::requestBeatAtTime(double beat, double atNtpSeconds, double quantum) {
    auto st = mImpl->link.captureAppSessionState();
    st.requestBeatAtTime(beat, mImpl->link.clock().micros(), quantum);
    double tempo = st.tempo();
    if (!(tempo >= 1.0)) tempo = 1.0;
    mImpl->link.commitAppSessionState(st);
    // Mirror the implied NTP beat origin so snapshot's beatAtTime math works
    // against NTP (Link stores in mClock domain, not NTP).
    const double newOrigin = atNtpSeconds - beat * 60.0 / tempo;
    SuperClockState* s = mImpl->clock.state();
    if (!s) return;
    s->beat_origin_ntp.store(doubleToBits(newOrigin), std::memory_order_relaxed);
}

void LinkSession::forceBeatAtTime(double beat, double atNtpSeconds, double quantum) {
    auto st = mImpl->link.captureAppSessionState();
    st.forceBeatAtTime(beat, mImpl->link.clock().micros(), quantum);
    double tempo = st.tempo();
    if (!(tempo >= 1.0)) tempo = 1.0;
    mImpl->link.commitAppSessionState(st);
    const double newOrigin = atNtpSeconds - beat * 60.0 / tempo;
    SuperClockState* s = mImpl->clock.state();
    if (!s) return;
    s->beat_origin_ntp.store(doubleToBits(newOrigin), std::memory_order_relaxed);
}

// ─── Enable / async enable ───────────────────────────────────────────────────

void LinkSession::setApplyVisibility(std::function<void(LinkVisibility)> apply) {
    mImpl->applyVisibility = std::move(apply);
}

void LinkSession::requestSetLinkEnabledAsync(bool enabled) {
    // Under the worker's mutex (see dtor) so the request can't be lost to a
    // notify that races the worker's park in wait().
    {
        std::lock_guard<std::mutex> lk(mImpl->deferredMtx);
        mImpl->deferredLinkEnableReq.store(enabled ? 1 : 0,
                                           std::memory_order_release);
    }
    mImpl->deferredCv.notify_all();
}

LinkSession::LinkVisibility LinkSession::lastNonOffVisibility() const {
    return mImpl->lastNonOffVisibility;
}

void LinkSession::setLastNonOffVisibility(LinkVisibility v) {
    mImpl->lastNonOffVisibility = v;
}

// ─── Visibility primitives ───────────────────────────────────────────────────

void LinkSession::prepareDisable() {
    mImpl->link.enableLinkAudio(false);
    mImpl->link.enable(false);
    auto* impl = mImpl.get();
    mImpl->link.callOnLinkThread([impl]() { impl->threadPriority.reset(); });
}

void LinkSession::enableWithPriority() {
    auto* impl = mImpl.get();
    mImpl->link.enable(true);
    mImpl->link.callOnLinkThread([impl]() { impl->threadPriority.setHigh(); });
    mImpl->link.enableLinkAudio(true);
}

void LinkSession::setLoopbackOnly(bool loopbackOnly) {
    linkLoopbackOnlyFlag().store(loopbackOnly, std::memory_order_relaxed);
}

bool LinkSession::isEnabled() const {
    return mImpl->link.isEnabled();
}

LinkSession::LinkVisibility LinkSession::getVisibility() const {
    if (!mImpl->link.isEnabled()) return LinkVisibility::Off;
    return linkLoopbackOnlyFlag().load(std::memory_order_relaxed)
        ? LinkVisibility::LoopbackOnly
        : LinkVisibility::NetworkWide;
}

// ─── Link-clock-domain RPC ───────────────────────────────────────────────────

int64_t LinkSession::clockMicros() const {
    return mImpl->link.clock().micros().count();
}

int64_t LinkSession::timeForIsPlayingMicros() const {
    return mImpl->link.captureAppSessionState().timeForIsPlaying().count();
}

double LinkSession::beatAtLinkTime(int64_t timeMicros, double quantum) const {
    return mImpl->link.captureAppSessionState().beatAtTime(
        std::chrono::microseconds{timeMicros}, quantum);
}

double LinkSession::phaseAtLinkTime(int64_t timeMicros, double quantum) const {
    return mImpl->link.captureAppSessionState().phaseAtTime(
        std::chrono::microseconds{timeMicros}, quantum);
}

int64_t LinkSession::timeAtBeatLinkMicros(double beat, double quantum) const {
    return mImpl->link.captureAppSessionState().timeAtBeat(beat, quantum).count();
}

// ─── Status ──────────────────────────────────────────────────────────────────

bool LinkSession::isStartStopSyncEnabled() const {
    return mImpl->link.isStartStopSyncEnabled();
}

size_t LinkSession::numPeers() const {
    return mImpl->link.numPeers();
}

std::vector<LinkSession::PeerInfo> LinkSession::listPeers() const {
    std::vector<PeerInfo> out;
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
    return out;
}

void LinkSession::publishLinkClockMetrics(PerformanceMetrics* m, double quantum) const {
    if (!m) return;
    // One realtime-safe, lock-free capture, read coherently for every Link field
    // below. This MUST use captureAudioSessionState() (the audio-thread variant):
    // the convenience getters tempo()/isPlaying() and the NTP-domain
    // beatAtTime()/phaseAtTime() each reach for captureAppSessionState(), which
    // takes a lock and must never run on the audio thread. Beat/phase are queried
    // in Link's own clock domain at "now", mirroring drainLinkAudioInputsToBuses /
    // the tempo-changed callback.
    const auto st = mImpl->link.captureAudioSessionState();
    const auto nowMicros = mImpl->link.clock().micros();

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
}

// ─── Peer name ─────────────────────────────────────────────────────────────

void LinkSession::setPeerName(const char* name) {
    std::string requested = name ? name : "";
    if (mImpl->peerNameCache == requested) return;
    mImpl->peerNameCache = std::move(requested);
    mImpl->link.setPeerName(mImpl->peerNameCache);
}

const char* LinkSession::peerName() const {
    return mImpl->peerNameCache.c_str();
}

// ─── Event callbacks (fired on Link's network thread; mirror into SAB) ───────

void LinkSession::setTempoChangedCallback(std::function<void(double)> cb) {
    // Mirror remote-peer tempo into the SAB and recompute beat_origin_ntp under
    // the new tempo so mirror-domain beatAtTime stays coherent with Link-domain
    // beatAtTime.
    auto* impl = mImpl.get();
    mImpl->link.setTempoCallback(
        [cb = std::move(cb), impl](const double bpmIn) {
            double bpm = bpmIn;
            if (!(bpm >= 1.0)) bpm = 1.0;
            SuperClockState* s = impl->clock.state();
            if (s) s->bpm.store(doubleToBits(bpm), std::memory_order_relaxed);
            auto st = impl->link.captureAppSessionState();
            const auto now = impl->link.clock().micros();
            constexpr double kBeatOriginQuantum = 4.0;
            const double currentBeat = st.beatAtTime(now, kBeatOriginQuantum);
            const double wallNow = wallClockNTP();
            const double newOrigin = wallNow - currentBeat * 60.0 / bpm;
            if (s) s->beat_origin_ntp.store(doubleToBits(newOrigin),
                                            std::memory_order_relaxed);
            // cb may be empty (shutdown's setTempoChangedCallback({})). Pass
            // clamped bpm so a corrupt peer's NaN doesn't leak out.
            if (cb) cb(bpm);
        });
}

void LinkSession::setNumPeersChangedCallback(std::function<void(std::size_t)> cb) {
    mImpl->link.setNumPeersCallback(
        [cb = std::move(cb)](const std::size_t n) { if (cb) cb(n); });
}

void LinkSession::setStartStopChangedCallback(std::function<void(bool, int64_t)> cb) {
    auto* impl = mImpl.get();
    mImpl->link.setStartStopCallback(
        [cb = std::move(cb), impl](const bool playing) {
            SuperClockState* s = impl->clock.state();
            if (s) {
                s->is_playing.store(playing ? 1u : 0u, std::memory_order_relaxed);
                // Mirror is_playing_at_ntp too so consumers reading both (e.g.
                // /superclock_get → beats-since-transport-start math) don't see
                // new is_playing paired with stale timestamp. wallClockNTP() is
                // the closest NTP-domain timestamp to "right now" we have on the
                // Link network thread.
                s->is_playing_at_ntp.store(doubleToBits(wallClockNTP()),
                                           std::memory_order_relaxed);
            }
            const auto t = impl->link.captureAppSessionState()
                                     .timeForIsPlaying().count();
            if (cb) cb(playing, t);
        });
}

// ─── Audio-thread accessors ──────────────────────────────────────────────────

ableton::LinkAudio& LinkSession::linkAudio() {
    return mImpl->link;
}

void* LinkSession::audioThreadLinkAudioPtr() {
    return &mImpl->link;
}

int64_t LinkSession::linkClockMicrosRaw() const {
    return mImpl->link.clock().micros().count();
}

#endif  // SUPERSONIC_LINK
