/*
 * LinkSession.h — Ableton Link clock-sync session.
 *
 * Owns the cross-machine session timeline: the ableton::LinkAudio instance, its
 * tempo / transport / peers callbacks, the deferred enable/disable worker
 * thread, the peer-name advertisement, and the loopback-only interface filter.
 * The Link Audio bus machinery (sinks / input subs / RT drain) is NOT here —
 * that is LinkAudioBridge, which borrows the LinkAudio instance this session
 * owns. The NTP time-source is TimeSource; Link-Audio orchestration stays in
 * SuperClockNative.
 *
 * Session mutators (setBpm / setIsPlaying / requestBeatAtTime / forceBeatAtTime)
 * and Link's own tempo/transport callbacks mirror the converged values back into
 * the SuperClockState SAB region that SuperClock core owns, so every snapshot
 * reader (including no-Link / WASM) sees the same data shape. The session
 * borrows the owning SuperClock& to reach that region via SuperClock::state().
 *
 * Two compile shapes selected by SUPERSONIC_LINK:
 *   defined   — LinkSession.cpp: real Ableton session + worker thread.
 *   undefined — the inline session-of-one (this header): thread-free,
 *               Ableton-free. setBpm / transport / beat-origin write straight to
 *               the SAB; isEnabled / getVisibility / isStartStopSyncEnabled read
 *               those flags back; peers / clock RPC return zero/empty.
 *
 * Dispatch is link-time-concrete (no vtable) so the RT-adjacent calls stay
 * branch-free at the call site.
 */
#pragma once

#include "SuperClock.h"
#include "clock_math.h"

#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

#if SUPERSONIC_LINK

#include <memory>
#include <string>

namespace ableton { class LinkAudio; }

class LinkSession {
public:
    // The owning SuperClock supplies the SAB mirror (SuperClock::state()); the
    // periodic tick is invoked off the RT thread (~every 250 ms) and drives MIDI
    // follower staleness — wired by SuperClock so the session never knows MIDI.
    explicit LinkSession(SuperClock& clock, std::function<void()> periodicTick);
    ~LinkSession();

    LinkSession(const LinkSession&) = delete;
    LinkSession& operator=(const LinkSession&) = delete;

    // Spawn the deferred worker. Deliberately NOT started in the constructor: the
    // worker's periodic tick reaches back through the owning SuperClock (mClock)
    // into SuperClock::mImpl, which is still being assigned by make_unique while
    // this LinkSession — a member of *Impl — is constructing. Starting it there is
    // a data race on mImpl (worker read vs main-thread write, no happens-before).
    // SuperClock calls this once from its constructor body, after mImpl is live.
    void startWorker();

    // Stop and join the deferred worker. Idempotent (a no-op once joined), so
    // the host can call it early in teardown and ~LinkSession's own join then
    // finds nothing to do. Must run before any sibling the worker reaches
    // (MidiTimelines via the periodic tick, LinkAudioBridge via applyVisibility)
    // is torn down.
    void stopWorker();

    using LinkVisibility = SuperClock::LinkVisibility;
    using PeerInfo       = SuperClock::PeerInfo;

    // ─── Session mutators (mirror into the SAB) ──────────────────────────────
    void setBpm(double bpm);
    void setIsPlaying(bool playing, double atNtpSeconds);
    void setStartStopSyncEnabled(bool enabled);
    void requestBeatAtTime(double beat, double atNtpSeconds, double quantum);
    void forceBeatAtTime(double beat, double atNtpSeconds, double quantum);

    // ─── Enable / async enable ───────────────────────────────────────────────
    // applyVisibility wires the deferred worker back to SuperClock's visibility
    // orchestrator (the worker may not touch Link Audio sinks directly).
    void setApplyVisibility(std::function<void(LinkVisibility)> apply);
    void requestSetLinkEnabledAsync(bool enabled);
    LinkVisibility lastNonOffVisibility() const;
    void           setLastNonOffVisibility(LinkVisibility v);

    // ─── Visibility primitives (composed by SuperClock::setLinkVisibility) ───
    // Tear-down order: Link Audio off, Link off, drop the network-thread
    // priority. Bring-up order: enable (interface filter already set), raise
    // priority, Link Audio on.
    void prepareDisable();
    void enableWithPriority();
    void setLoopbackOnly(bool loopbackOnly);
    bool           isEnabled() const;
    LinkVisibility getVisibility() const;

    // ─── Link-clock-domain RPC ───────────────────────────────────────────────
    int64_t clockMicros() const;
    int64_t timeForIsPlayingMicros() const;
    double  beatAtLinkTime(int64_t timeMicros, double quantum) const;
    double  phaseAtLinkTime(int64_t timeMicros, double quantum) const;
    int64_t timeAtBeatLinkMicros(double beat, double quantum) const;

    // ─── Status ──────────────────────────────────────────────────────────────
    bool   isStartStopSyncEnabled() const;
    size_t numPeers() const;
    std::vector<PeerInfo> listPeers() const;

    // Mirror the live Link clock readouts (peers / tempo / beat / phase /
    // playing) into the dashboard metrics. RT-safe (one lock-free
    // captureAudioSessionState). No-op without Link. Link Audio stream-health is
    // written separately by SuperClockNative from the bus bridge.
    void publishLinkClockMetrics(PerformanceMetrics* m, double quantum) const;

    // ─── Peer name ───────────────────────────────────────────────────────────
    void        setPeerName(const char* name);
    const char* peerName() const;

    // ─── Event callbacks (fired on Link's network thread; mirror into SAB) ───
    void setTempoChangedCallback(std::function<void(double)> cb);
    void setNumPeersChangedCallback(std::function<void(std::size_t)> cb);
    void setStartStopChangedCallback(std::function<void(bool, int64_t)> cb);

    // ─── Audio-thread accessors ──────────────────────────────────────────────
    // The borrowed ableton::LinkAudio instance — for LinkAudioBridge, LinkUGen
    // plugins (audioThreadLinkAudioPtr) and the Link-clock time-source read in
    // SuperClockNative (linkAudioHostMicros). Cast at the call site keeps this
    // header free of Ableton types where it can be.
    ableton::LinkAudio& linkAudio();
    void* audioThreadLinkAudioPtr();
    int64_t linkClockMicrosRaw() const;  // link.clock().micros().count()

private:
    struct Impl;
    std::unique_ptr<Impl> mImpl;
};

#else  // !SUPERSONIC_LINK

// Session-of-one: no Ableton, no thread. Mutators write the SAB mirror directly
// (the same fields the Ableton path converges onto); isEnabled / getVisibility /
// isStartStopSyncEnabled read those flags back; peer / clock reads are zero/empty.
// Header-inline — no separate TU.
class LinkSession {
public:
    using LinkVisibility = SuperClock::LinkVisibility;
    using PeerInfo       = SuperClock::PeerInfo;

    explicit LinkSession(SuperClock& clock, std::function<void()> /*periodicTick*/)
        : mClock(clock) {}

    LinkSession(const LinkSession&) = delete;
    LinkSession& operator=(const LinkSession&) = delete;

    // Session-of-one has no worker thread.
    void startWorker() {}
    void stopWorker() {}

    void setBpm(double bpm) {
        if (SuperClockState* s = mClock.state())
            s->bpm.store(supersonic::doubleToBits(bpm), std::memory_order_relaxed);
    }
    void setIsPlaying(bool playing, double atNtpSeconds) {
        SuperClockState* s = mClock.state();
        if (!s) return;
        s->is_playing_at_ntp.store(supersonic::doubleToBits(atNtpSeconds),
                                   std::memory_order_relaxed);
        s->is_playing.store(playing ? 1u : 0u, std::memory_order_relaxed);
    }
    void setStartStopSyncEnabled(bool enabled) {
        SuperClockState* s = mClock.state();
        if (!s) return;
        if (enabled) s->flags.fetch_or(SC_FLAG_START_STOP_SYNC,  std::memory_order_relaxed);
        else         s->flags.fetch_and(~SC_FLAG_START_STOP_SYNC, std::memory_order_relaxed);
    }
    void requestBeatAtTime(double beat, double atNtpSeconds, double /*quantum*/) {
        SuperClockState* s = mClock.state();
        if (!s) return;
        const double bpm = supersonic::bitsToDouble(s->bpm.load(std::memory_order_relaxed));
        const double newOrigin = supersonic::originFor(beat, atNtpSeconds, bpm);
        s->beat_origin_ntp.store(supersonic::doubleToBits(newOrigin), std::memory_order_relaxed);
    }
    // No peers in session-of-one — identical to requestBeatAtTime.
    void forceBeatAtTime(double beat, double atNtpSeconds, double quantum) {
        requestBeatAtTime(beat, atNtpSeconds, quantum);
    }

    void setApplyVisibility(std::function<void(LinkVisibility)>) {}
    void requestSetLinkEnabledAsync(bool) {}
    LinkVisibility lastNonOffVisibility() const { return LinkVisibility::LoopbackOnly; }
    void           setLastNonOffVisibility(LinkVisibility) {}

    void prepareDisable() {}
    void enableWithPriority() {}
    void setLoopbackOnly(bool) {}
    bool           isEnabled() const {
        const SuperClockState* s = mClock.state();
        return s && (s->flags.load(std::memory_order_relaxed) & SC_FLAG_LINK_ENABLED) != 0u;
    }
    LinkVisibility getVisibility() const {
        return isEnabled() ? LinkVisibility::LoopbackOnly : LinkVisibility::Off;
    }

    // Monotonic steady-clock micros so callers needing a "now" in the same
    // domain as future clockMicros() calls still get a sane value.
    int64_t clockMicros() const {
        return std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
    }
    int64_t timeForIsPlayingMicros() const { return 0; }
    double  beatAtLinkTime(int64_t, double) const { return 0.0; }
    double  phaseAtLinkTime(int64_t, double) const { return 0.0; }
    int64_t timeAtBeatLinkMicros(double, double) const { return 0; }

    bool   isStartStopSyncEnabled() const {
        const SuperClockState* s = mClock.state();
        return s && (s->flags.load(std::memory_order_relaxed) & SC_FLAG_START_STOP_SYNC) != 0u;
    }
    size_t numPeers() const { return 0; }
    std::vector<PeerInfo> listPeers() const { return {}; }

    void publishLinkClockMetrics(PerformanceMetrics*, double) const {}

    void        setPeerName(const char*) {}
    const char* peerName() const { return ""; }

    void setTempoChangedCallback(std::function<void(double)>) {}
    void setNumPeersChangedCallback(std::function<void(std::size_t)>) {}
    void setStartStopChangedCallback(std::function<void(bool, int64_t)>) {}

    void* audioThreadLinkAudioPtr() { return nullptr; }
    int64_t linkClockMicrosRaw() const { return 0; }

private:
    SuperClock& mClock;
};

#endif  // SUPERSONIC_LINK
