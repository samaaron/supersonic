/*
 * SuperClock.h — engine session-timeline service.
 *
 * Owns the engine's view of the Link session: tempo, transport, beat
 * origin, and audio-thread-derived NTP. Native wraps ableton::LinkAudio
 * (real cross-machine sync + audio sharing) when SUPERSONIC_LINK is set;
 * otherwise falls back to local seqlock-protected state. WASM always
 * uses the local-state path (no UDP in the browser).
 *
 * Same public API on both builds — callers don't branch on platform.
 */
#pragma once

#include "shared_memory.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

// ─── SuperClock ──────────────────────────────────────────────────────────────
class SuperClock {
public:
    SuperClock();
    ~SuperClock();

    SuperClock(const SuperClock&) = delete;
    SuperClock& operator=(const SuperClock&) = delete;
    SuperClock(SuperClock&&) = delete;
    SuperClock& operator=(SuperClock&&) = delete;

    // ─── Teardown (app-thread) ────────────────────────────────────────────
    // Stop and join the session's background worker (the ~250 ms MIDI-staleness
    // tick / async Link enable). Idempotent. The host MUST call this before
    // tearing down anything the worker reaches — the SHM arena the clock state
    // is bound into, the Link Audio bus — because the worker drives MIDI
    // staleness through the clock and would otherwise run against freed state in
    // the window before ~SuperClock joins it. No-op on thread-free builds.
    void stopBackgroundWork();

    // ─── Session mutators (app-thread) ────────────────────────────────────

    void setBpm(double bpm, double atNtpSeconds);
    void setIsPlaying(bool playing, double atNtpSeconds);
    void setLinkEnabled(bool enabled);

    // Audio-thread-safe variant of setLinkEnabled; defers the
    // blocking work to a SuperClock-owned worker thread.
    void requestSetLinkEnabledAsync(bool enabled);
    void setStartStopSyncEnabled(bool enabled);
    void requestBeatAtTime(double beat, double atNtpSeconds, double quantum);
    void forceBeatAtTime(double beat, double atNtpSeconds, double quantum);

    // ─── Session-state getters (app-thread) ───────────────────────────────

    double getBpm() const;
    bool   isPlaying() const;
    double getBeatOriginNtp() const;
    double getIsPlayingAtNtp() const;
    bool   isLinkEnabled() const;
    bool   isStartStopSyncEnabled() const;
    size_t numPeers() const;

    // ─── Beat math (app-thread) ───────────────────────────────────────────
    // Pure functions of (bpm, beat_origin). Reads each field independently
    // — for coherent reads on the audio thread use captureSessionState().

    double beatAtTime(double ntpSeconds, double quantum) const;
    double phaseAtTime(double ntpSeconds, double quantum) const;
    double timeAtBeat(double beat, double quantum) const;

    // ─── Link-clock-domain RPC (app-thread) ──────────────────────────────
    // Direct passthroughs to Link's clock + session-state API. Used by
    // OSC clients (e.g. Sonic Pi's LinkAPI) replicating sp_link's
    // surface. Return 0 / defaults on Link-off builds.

    int64_t linkClockMicros() const;
    int64_t timeForIsPlayingMicros() const;
    double  beatAtLinkTime(int64_t timeMicros, double quantum) const;
    double  phaseAtLinkTime(int64_t timeMicros, double quantum) const;
    int64_t timeAtBeatLinkMicros(double beat, double quantum) const;

    // ─── MIDI-clock follower timelines (native; stubs on WASM) ───────────
    // SuperClock owns a fixed registry of midi:<port> follower timelines,
    // separate from the Link timeline. The MIDI subsystem feeds tempo /
    // transport per port; OSC clients read them via /clock/midi:<port>/*.
    // Slot assignment, primary selection, and staleness all live here.
    //
    // Timeline id: 0 = Link (routes to the getters/RPC above); 1..K =
    // midi slots. Beat math for midi timelines is in the Link-clock micros
    // domain, so OSC RPCs answer identically to the Link timeline.

    // Find-or-allocate the slot for a port; idempotent. `normalized` is the
    // OSC-safe handle (match key + /clock/midi:<handle>/ address segment);
    // `raw` is the original OS device name, kept for display. Returns 1..K,
    // or -1 if the registry is full. Fed off the RT thread (MIDI subsystem).
    int  claimMidiTimeline(const char* normalized, const char* raw);
    void freeMidiTimeline(int id);

    // Resolve a /clock/<tl>/ name to an id: "" / "link" → 0; "midi" (bare) →
    // the primary midi slot; "midi:<port>" → that port's slot. Returns -1 for an
    // unclaimed port or a malformed name (read methods treat id -1 as a 60 BPM
    // placeholder; resolve never auto-claims — claims happen on the feed path).
    int  resolveTimeline(const char* name) const;

    // Write-path variant: resolves like the above, but a "midi:<port>" name
    // claims the slot if it hasn't clocked yet (so the OSC manual-set/transport
    // path doesn't special-case the midi: grammar). Bare "midi" can't claim.
    int  resolveOrClaimTimeline(const char* name);

    // Live clock feed: one 0xF8 pulse at OS timestamp `tsUs`. The beat is the
    // exact pulse count; the tempo is smoothed separately for interpolation.
    void midiTimelinePulse(int id, uint64_t tsUs);
    // Manual tempo set (OSC / unfed placeholder): advances beat continuously;
    // a live clock's pulses override it.
    void setMidiTimelineTempo(int id, double bpm);
    void setMidiTimelineTransport(int id, int kind, double beat);

    // Staleness sweep (called periodically off the RT thread): freezes
    // tempo + marks stale after a feed gap, frees the slot after grace.
    void tickMidiStaleness();

    // Timeline-parameterised reads (id 0 = Link → the methods above).
    double  timelineBpm(int id) const;
    bool    timelineIsPlaying(int id) const;
    // Whether a transport event (START or SPP) has defined the timeline's beat
    // origin. Without one, beats are arbitrary 24-pulse groupings from the
    // first pulse seen, so bar phase is meaningless. Link (id 0) is always
    // anchored — its session grid exists independent of transport.
    bool    timelineIsAnchored(int id) const;
    int64_t timelineTimeForIsPlayingMicros(int id) const;
    double  timelineBeatAtLinkTime(int id, int64_t timeMicros, double quantum) const;
    double  timelinePhaseAtLinkTime(int id, int64_t timeMicros, double quantum) const;
    int64_t timelineTimeAtBeatLinkMicros(int id, double beat, double quantum) const;

    // NTP (wall-clock) domain, for scheduling clients (Spider). The Link clock is
    // a per-boot monotonic clock (mach_absolute_time on macOS) — an arbitrary
    // epoch that freezes during system sleep — so a remote client mapping it to
    // wall time must carry a measured, drift-prone offset. These convert at the
    // engine, sampling both clocks in-process microseconds apart (exact, always
    // fresh), so the client needs only the fixed NTP<->Unix epoch constant.
    // ntpNowMicros() is the current wall time in NTP micros.
    int64_t ntpNowMicros() const;
    int64_t linkMicrosToNtpMicros(int64_t linkMicros) const;
    int64_t ntpMicrosToLinkMicros(int64_t ntpMicros) const;

    // Enumeration snapshot for /clock/timelines/get.
    struct TimelineInfo {
        std::string name;            // wire identity: "link" | "midi:<handle>"
        std::string raw;             // original OS device name (display); "link" for Link
        double      bpm{0.0};
        bool        clocking{false};
        bool        stale{false};
        bool        primary{false};
    };
    std::vector<TimelineInfo> listTimelines() const;

    // Fired (off RT) when the timeline set changes — add / remove / stale /
    // primary. At most one callback; setting replaces. Never fires on WASM.
    void setTimelinesChangedCallback(std::function<void()> cb);

    // ─── Link event callbacks (registered by app code; fired on Link's
    // network thread). At most one callback per kind; setting replaces.

    void setTempoChangedCallback(std::function<void(double bpm)> cb);
    void setNumPeersChangedCallback(std::function<void(std::size_t)> cb);
    void setStartStopChangedCallback(std::function<void(bool playing, int64_t atLinkMicros)> cb);

    // ─── Link Audio: visibility + publish + peer name ────────────────────

    // Three-state network visibility for Link + Link Audio:
    //   Off          — no peer discovery, no audio sharing
    //   LoopbackOnly — discovery + audio sharing on lo0 only (same-machine
    //                  peers can find us; LAN cannot)
    //   NetworkWide  — full Link: discoverable on every UP interface
    // Default at construction is Off — engines must opt in.
    enum class LinkVisibility { Off = 0, LoopbackOnly = 1, NetworkWide = 2 };
    void           setLinkVisibility(LinkVisibility v);
    LinkVisibility getLinkVisibility() const;

    // Link Audio publish gate. Orthogonal to visibility — set true to
    // advertise + send our audio channels on the mesh, false to stay
    // silent while the mesh is up. Default false; explicit opt-in.
    void setLinkAudioPublish(bool publish);
    bool isLinkAudioPublishEnabled() const;

    // Peer name shown to other Link participants. Truncated to 256 chars
    // by Link. Stable NUL-terminated pointer valid until next setPeerName.
    void        setPeerName(const char* name);
    const char* peerName() const;

    // ─── Link Audio: peer + channel discovery ────────────────────────────

    struct PeerInfo {
        std::string nodeId;            // 16-hex-char unique Link node identifier
        std::string gatewayIp;         // local interface they were discovered on
        std::string measurementIp;     // peer's ping/pong endpoint IP
        uint16_t    measurementPort{0};
        std::string audioIp;           // peer's Link Audio endpoint IP, "" if none
        uint16_t    audioPort{0};
        bool        isLoopback{false};
    };
    std::vector<PeerInfo> listPeers() const;

    struct LinkAudioChannel {
        std::string channelId;
        std::string channelName;
        std::string peerId;
        std::string peerName;
    };
    std::vector<LinkAudioChannel> listLinkAudioChannels() const;

    // ─── Link Audio: input subscription ──────────────────────────────────

    // Subscribe to one peer's named audio channel; received samples
    // are written into two consecutive buses (busIdx, busIdx+1).
    // Always stereo: Link Audio caps numChannels at 2 (mono or
    // stereo per channel; commit() rejects anything else), and the
    // renderer mirrors mono sources to both buses so the caller can
    // always claim 2 buses regardless of the source's actual layout.
    // Multiple subscriptions are active concurrently; (peerName,
    // channelName) is the replacement key. Returns true if a matching
    // channel was found at call time.
    bool addLinkAudioInput(const char* peerName,
                           const char* channelName,
                           uint32_t busIdx);
    void removeLinkAudioInput(const char* peerName, const char* channelName);
    void clearLinkAudioInputs();

    // Per-subscription playback lookahead in wall-clock seconds —
    // equivalent to Live's per-track latency slider. Values outside
    // [0, kMaxLinkAudioInputLatencySeconds] are rejected; the cap
    // matches the renderer's ring capacity. Default lookahead is 50 ms.
    // Returns true iff the (peer, channel) sub exists AND seconds is
    // in range.
    static constexpr double kMaxLinkAudioInputLatencySeconds = 2.0;
    bool setLinkAudioInputLatencySeconds(const char* peerName,
                                          const char* channelName,
                                          double seconds);

    enum class LinkAudioConnectionState {
        NotSubscribed = 0, Connecting = 1, Connected = 2, Dropout = 3
    };
    struct LinkAudioInputStatus {
        std::string              peerName;
        std::string              channelName;
        uint32_t                 busIdx{0};
        uint32_t                 sampleRate{0};
        // Source's actual channel count (1 or 2). 0 until the first
        // buffer arrives. Subscription always claims 2 buses regardless.
        uint32_t                 sourceNumChannels{0};
        float                    bufferedSeconds{0.0f};
        LinkAudioConnectionState state{LinkAudioConnectionState::NotSubscribed};
        uint64_t                 droppedSourceBuffers{0};   // our queue saturated
        uint64_t                 networkGapBuffers{0};      // upstream loss (Info::count gaps)
        uint64_t                 totalSourceBufferCalls{0}; // diagnostic: raw onSourceBuffer invocations
        uint64_t                 duplicateCountCalls{0};    // diagnostic: invocations with repeated count
        double                   latencySeconds{0.0};       // current playback lookahead
    };
    std::vector<LinkAudioInputStatus> listLinkAudioInputs() const;

    // ─── Link Audio: auxiliary sinks (multi-channel publish) ─────────────
    //
    // One additional named sink (beyond the auto-created main sink at
    // outputs 0/1) bound to a user-chosen bus range. Audio thread taps
    // busPool[busIdx..busIdx+numChans) each block and publishes as
    // channel `name`.

    struct ActiveSinkInfo {
        std::string name;
        uint32_t    busIdx{0};
        uint32_t    numChannels{0};
        bool        hasSubscriber{false};
    };
    bool addLinkAudioSink(const char* name, uint32_t busIdx, uint32_t numChannels);
    void removeLinkAudioSink(const char* name);
    std::vector<ActiveSinkInfo> listLinkAudioSinks() const;

    // ─── Audio-thread API (RT-safe) ───────────────────────────────────────

    // `now()` is the app-thread read: returns the latest audio-thread NTP,
    // cached by the most recent update call. Both builds.
    //
    // `nowAt(audioCurrentTime)` is the WASM worklet's audio-thread entry
    // point: computes NTP from the supplied AudioContext currentTime and
    // publishes it to the cache. On native, audio-thread NTP comes from
    // the IIR — `nowAt` ignores its argument and returns `now()`.

    double now() const;
    double nowAt(double audioCurrentTime) const;

    // App-thread wall-clock NTP entry point. Native returns wallClockNTP()
    // directly; WASM has no fresh wall clock and returns 0.
    double wallNow() const;

    // Audio-thread time-base. Native runs one IIR step per callback;
    // WASM evaluates the SAB formula.
    double updateAudioThreadNTP(double samplePosition,
                                double sampleRate,
                                double audioCurrentTime = 0.0);
    void   resetAudioThreadTime(double samplePosition, double sampleRate);

    // Drift-corrected host time (Link clock domain, µs) for the Link Audio
    // receive resampler; rejects audio-thread wake jitter. Audio-thread only,
    // returns 0 on Link-off builds.
    int64_t linkAudioHostMicros(double samplePosition, double sampleRate);

    // Freewheel clock mode: derive the audio-thread NTP purely from sample
    // position, skipping the wall-clock drift IIR in updateAudioThreadNTP.
    // For deterministic offline/test rendering — the headless driver thread
    // can be preempted by the OS on a busy machine, and chasing that as
    // "drift" injects scheduling jitter that real hardware (driven by the
    // device callback) never sees. Off by default; real devices and the
    // headless fallback keep drift compensation.
    void   setFreewheelClock(bool enabled);

    // ─── Link Audio audio-thread API (RT-safe) ───────────────────────────

    // Drain each active input subscription's receive renderer (one
    // beat-time-aligned block per call, 4-beat lookahead, cubic
    // interp) into its assigned bus. RT-safe (try_lock on the
    // subscription list; lock-free fast path when no subscriptions).
    // sampleRate = engine output rate; hostMicrosForBufferBegin =
    // audio framework's playback timestamp for the first frame of
    // this block (0 falls back to link.clock().micros()).
    void drainLinkAudioInputsToBuses(float* busPool,
                                     uint32_t blockSize,
                                     uint32_t numBuses,
                                     uint32_t sampleRate,
                                     uint64_t hostMicrosForBufferBegin = 0);

    // Tap each active aux sink's bus range and publish on Link Audio.
    // RT-safe (try_lock; skips block if the sink list is mid-mutation).
    void publishAuxSinks(const float* busPool,
                         uint32_t blockSize,
                         uint32_t numBuses,
                         uint32_t sampleRate,
                         uint64_t hostMicrosForBufferBegin,
                         double quantum = 4.0);

    // Publish one main-sink block to Link Audio. Stereo iff
    // rightChannel != nullptr. Returns true if a remote subscriber is
    // present and audio actually shipped. Same hostMicros semantics as
    // drainLinkAudioInputsToBuses.
    bool publishAudioBlock(const float* leftChannel,
                           const float* rightChannel,
                           size_t numFrames,
                           uint32_t sampleRate,
                           uint64_t hostMicrosForBufferBegin = 0,
                           double quantum = 4.0);

    // Mirror the current Link clock + Link Audio stream-health into the
    // dashboard metrics (relaxed atomics). RT-safe; called once per callback.
    // No-op on WASM. `m` may be null (skipped).
    void publishLinkMetrics(PerformanceMetrics* m, double quantum = 4.0);

    // Mirror the current SuperClock readout (tempo/beat/phase/playing) into the
    // cross-platform clock metrics (slots 65-68). Reads the SuperClockState SAB
    // mirror directly (relaxed atomics) + inline beat math — RT-safe and
    // identical on web and native, independent of Link. Called once per audio
    // callback with the audio-thread NTP. `m` may be null (skipped). Shared
    // implementation in SuperClock.cpp (no platform override).
    void publishClockMetrics(PerformanceMetrics* m, double ntpNow, double quantum = 4.0);

    // ─── Shared-memory state accessor (RT-safe reads) ────────────────────
    // Underlying SuperClockState — the engine's shared arena region on BOTH
    // WASM (bound at superclock_wasm_init) and native (bound at
    // bindStateToShm), so the clock has one identical SHM shape on every build.
    // Atomic loads through this pointer are RT-safe; the audio thread should
    // prefer them over getBpm()/isPlaying(), which on native+Link route through
    // Link::captureAppSessionState() (documented "Realtime-safe: no").
    SuperClockState*       state();
    const SuperClockState* state() const;

    // Point the clock state at a shared arena's SUPERCLOCK_STATE region (copying
    // current state into it) so the SHM has one shape on every build. Native
    // binds the cross-process arena at engine init; the worklet (WASM) binds its
    // SAB region at boot via superclock_wasm_init. Called once before the audio
    // thread runs — no concurrency.
    void bindStateToShm(SuperClockState* region);

#if SUPERSONIC_WORKLET_CLOCK
    // Worklet builds only: hand the worklet TimeSource its SAB offset pointers
    // (NTP start / drift µs / global ms) so nowAt() can evaluate the SAB time
    // formula. Wired by superclock_wasm_init at boot.
    void bindWorkletClock(const double* ntpStartTime,
                          const std::atomic<int32_t>* driftOffset,
                          const std::atomic<int32_t>* globalOffset);
#endif

    // Audio-thread accessor for the underlying LinkAudio instance.
    // Used by scsynth plugins (LinkTempo / LinkPhase / LinkJump
    // UGens) that need captureAudioSessionState from the audio
    // thread. Returns nullptr on no-Link builds. The void* return
    // keeps this header free of ableton headers; callers cast.
    void* audioThreadLinkAudioPtr();

private:
    struct Impl;
    std::unique_ptr<Impl> mImpl;

    // Platform-specific Link↔NTP domain mapping. Used by the shared
    // Link-clock-domain RPC implementations to convert between Link's
    // steady micros and NTP seconds.
    double  linkMicrosToNtpSeconds(int64_t linkMicros) const;
    int64_t ntpSecondsToLinkMicros(double ntpSeconds) const;
};

// Active SuperClock pointer for the /superclock_get OSC verb (queryable
// in both SAB and PM modes). Published at engine boot, single-publisher
// — multi-engine native is not supported.
extern std::atomic<SuperClock*> g_active_superclock;
