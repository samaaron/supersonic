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

    // ─── Session mutators (app-thread) ────────────────────────────────────

    void setBpm(double bpm, double atNtpSeconds);
    void setIsPlaying(bool playing, double atNtpSeconds);
    void setLinkEnabled(bool enabled);
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

    // ─── SAB mirror accessor (RT-safe reads) ─────────────────────────────
    // Underlying SuperClockState — SAB region on WASM, private mirror
    // on native. Atomic loads through this pointer are RT-safe; the
    // audio thread should prefer them over getBpm()/isPlaying(), which
    // on native+Link route through Link::captureAppSessionState()
    // (documented "Realtime-safe: no").
    SuperClockState*       state();
    const SuperClockState* state() const;

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
