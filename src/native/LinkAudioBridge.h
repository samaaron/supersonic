/*
 * LinkAudioBridge.h — Link Audio ↔ scsynth bus-pool bridge.
 *
 * Owns the audio side of Ableton Link: the publish-side main sink and the
 * user-added aux sinks, the receive-side per-channel input subscriptions, and
 * the RT-thread drain/publish that moves samples between Link's channels and
 * scsynth's audio bus pool. Clock sync (tempo / transport / peers) is NOT here
 * — that stays in SuperClockNative with the ableton::LinkAudio instance, which
 * the bridge borrows by reference.
 *
 * Two compile shapes selected by SUPERSONIC_LINK_AUDIO (== SUPERSONIC_LINK &&
 * SUPERSONIC_SYNTH):
 *   1 — the real bridge (this header declares; LinkAudioBridge.cpp defines).
 *       Needs both the Link session and scsynth's bus pool (touch_audio_bus).
 *   0 — an inline no-op bridge: every method returns empty/false. Lets a
 *       Link-on / synth-off build still construct a SuperClock without an
 *       audio-bus substrate. No separate TU; the no-op lives in this header.
 *
 * The RT-thread methods (drainInputsToBuses / publishAuxSinks /
 * publishAudioBlock) are concrete (no vtable) and keep their try_lock skip-on-
 * contention behaviour. The audio-framework host-micros timestamp is passed in
 * per call by the audio-thread caller (Link-clock domain); the bridge never
 * reaches back into SuperClock for it.
 */
#pragma once

#include "SuperClock.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// SUPERSONIC_LINK_AUDIO mirrors SuperClockNative's selection: Link Audio (audio
// streaming into/out of scsynth's bus pool) exists only when both Ableton Link
// and the scsynth engine are compiled.
#if defined(SUPERSONIC_LINK) && SUPERSONIC_SYNTH
#define SUPERSONIC_LINK_AUDIO 1
#else
#define SUPERSONIC_LINK_AUDIO 0
#endif

#if SUPERSONIC_LINK_AUDIO

#include "native/vendor/LinkAudioInputRenderer.hpp"

#include <ableton/LinkAudio.hpp>

#include <atomic>
#include <memory>
#include <mutex>
#include <optional>

class LinkAudioBridge {
public:
    explicit LinkAudioBridge(ableton::LinkAudio& link) : mLink(link) {}

    LinkAudioBridge(const LinkAudioBridge&) = delete;
    LinkAudioBridge& operator=(const LinkAudioBridge&) = delete;

    // ─── Publish gate + channel discovery ────────────────────────────────
    // setPublishEnabled reflects immediately when linkEnabled (creates/destroys
    // the main sink + clears aux sinks); otherwise the next ensureMainSink (on
    // the next non-Off visibility transition) creates the sink.
    void setPublishEnabled(bool publish, bool linkEnabled);
    bool isPublishEnabled() const { return mAudioPublishEnabled; }
    std::vector<SuperClock::LinkAudioChannel> listChannels() const;

    // ─── Input subscriptions ──────────────────────────────────────────────
    bool addInput(const char* peerName, const char* channelName, uint32_t busIdx);
    void removeInput(const char* peerName, const char* channelName);
    void clearInputs();
    bool setInputLatencySeconds(const char* peerName, const char* channelName,
                                double seconds);
    std::vector<SuperClock::LinkAudioInputStatus> listInputs() const;

    // ─── Auxiliary sinks ──────────────────────────────────────────────────
    bool addSink(const char* name, uint32_t busIdx, uint32_t numChannels);
    void removeSink(const char* name);
    std::vector<SuperClock::ActiveSinkInfo> listSinks() const;

    // ─── RT-thread: drain + publish ───────────────────────────────────────
    void drainInputsToBuses(float* busPool, uint32_t blockSize, uint32_t numBuses,
                            uint32_t sampleRate, uint64_t hostMicrosForBufferBegin);
    void publishAuxSinks(const float* busPool, uint32_t blockSize, uint32_t numBuses,
                         uint32_t sampleRate, uint64_t hostMicrosForBufferBegin,
                         double quantum);
    bool publishAudioBlock(const float* leftChannel, const float* rightChannel,
                           size_t numFrames, uint32_t sampleRate,
                           uint64_t hostMicrosForBufferBegin, double quantum);

    // ─── Visibility-change support (called by SuperClock::setLinkVisibility) ─
    // Clear all subs / aux sinks / the main sink — they bind to the current
    // Link substrate's session-scoped channelIds, so they must drop before the
    // Link gateway tears down and be re-added after the next non-Off transition.
    void resetForVisibilityChange();
    // Recreate the main sink if publishing is enabled (called after Link comes
    // back up). No-op when publish is off.
    void ensureMainSink();

    // ─── Metrics (read by SuperClock::publishLinkMetrics) ─────────────────
    uint32_t underruns() const {
        return mLinkAudioUnderruns.load(std::memory_order_relaxed);
    }
    // RT-safe try_lock reads of the aux-sink count and aggregated input health.
    // Each returns false (writing nothing) when the lock is contended.
    bool tryReadSinkCount(uint32_t& outCount) const;
    struct InputHealth {
        uint32_t inChannels{0};
        uint32_t streamRate{0};
        int32_t  driftPpm{0};
        uint32_t bufferedMs{0};
    };
    bool tryReadInputHealth(InputHealth& out) const;

private:
    // Up to 2048-frame stereo blocks; scsynth normally runs at 128.
    static constexpr size_t kSinkMaxSamples = 4096;

    // Audio-channel name for the main sink (Live displays one row per channel
    // under each peer). "Main" matches Live's convention for its primary output.
    std::string mChannelNameCache{"Main"};

    ableton::LinkAudio& mLink;

    std::optional<ableton::LinkAudioSink> mSink;
    // Serialises sink reset/emplace against the audio thread's publishAudioBlock
    // dereference (audio thread uses try_lock).
    mutable std::mutex mSinkMutex;

    // Audio-publish gate. False = don't create the main sink even if the Link
    // mesh is up. App must explicitly setPublishEnabled(true).
    bool mAudioPublishEnabled{false};

    // One subscription per Link channel. Multiple are active concurrently;
    // (peerName, channelName) is the replacement key. Each owns one bus pair.
    struct InputSubscription {
        std::unique_ptr<supersonic_link::LinkAudioInputRenderer<ableton::LinkAudio>> renderer;
        uint32_t    busIdx{0};
        std::string peerName;
        std::string channelName;
        // Distinguishes a re-arm of the same channel from a peer rejoin that
        // publishes a new id. Same-id replacements reuse the existing renderer
        // so diagnostic counters survive.
        ableton::ChannelId channelId{};
    };
    std::vector<InputSubscription> mInputSubs;
    // Lock-free fast path for the empty case.
    std::atomic<size_t> mInputSubCount{0};
    // Serialises mInputSubs mutation against the audio thread's
    // drainInputsToBuses (try_lock) and the app-thread listInputs (blocking).
    mutable std::mutex mInputSubMutex;

    // Auxiliary sinks bound to user-chosen bus ranges. Mutex covers the vector
    // and the per-entry hasSubscriber flag; audio thread uses try_lock and
    // skips the block on contention.
    struct ActiveSink {
        std::string            name;
        uint32_t               busIdx;
        uint32_t               numChannels;
        ableton::LinkAudioSink sink;
        bool                   hasSubscriber{false};
    };
    mutable std::mutex      mAuxSinksMutex;
    std::vector<ActiveSink> mAuxSinks;
    // Lock-free fast path for the empty case.
    std::atomic<size_t>     mAuxSinkCount{0};

    // Audio-thread scratch for drainInputsToBuses. Held on the bridge to avoid
    // 64 KiB of stack per call (some Windows ASIO threads have <128 KiB stacks).
    // Single-thread access.
    static constexpr size_t kDrainScratchFrames = 4096;
    double mDrainScratchL[kDrainScratchFrames]{};
    double mDrainScratchR[kDrainScratchFrames]{};

    // Cumulative Link Audio receive underruns (a block the renderer couldn't
    // fully fill). Bumped in drainInputsToBuses, mirrored to metrics.
    std::atomic<uint32_t> mLinkAudioUnderruns{0};
};

#else  // !SUPERSONIC_LINK_AUDIO

// No-op bridge: a Link-on / synth-off build (no scsynth bus pool) constructs
// this and every Link Audio call inertly returns empty/false. Header-only —
// no separate TU. The publish-gate flag is tracked so SuperClock keeps its
// SAB-parity behaviour, but no sink/renderer machinery exists.
class LinkAudioBridge {
public:
    LinkAudioBridge() = default;

    LinkAudioBridge(const LinkAudioBridge&) = delete;
    LinkAudioBridge& operator=(const LinkAudioBridge&) = delete;

    void setPublishEnabled(bool publish, bool) { mAudioPublishEnabled = publish; }
    bool isPublishEnabled() const { return mAudioPublishEnabled; }
    std::vector<SuperClock::LinkAudioChannel> listChannels() const { return {}; }

    bool addInput(const char*, const char*, uint32_t) { return false; }
    void removeInput(const char*, const char*) {}
    void clearInputs() {}
    bool setInputLatencySeconds(const char*, const char*, double) { return false; }
    std::vector<SuperClock::LinkAudioInputStatus> listInputs() const { return {}; }

    bool addSink(const char*, uint32_t, uint32_t) { return false; }
    void removeSink(const char*) {}
    std::vector<SuperClock::ActiveSinkInfo> listSinks() const { return {}; }

    void drainInputsToBuses(float*, uint32_t, uint32_t, uint32_t, uint64_t) {}
    void publishAuxSinks(const float*, uint32_t, uint32_t, uint32_t, uint64_t, double) {}
    bool publishAudioBlock(const float*, const float*, size_t, uint32_t, uint64_t,
                           double) { return false; }

    void resetForVisibilityChange() {}
    void ensureMainSink() {}

    uint32_t underruns() const { return 0; }
    bool tryReadSinkCount(uint32_t&) const { return false; }
    struct InputHealth {
        uint32_t inChannels{0};
        uint32_t streamRate{0};
        int32_t  driftPpm{0};
        uint32_t bufferedMs{0};
    };
    bool tryReadInputHealth(InputHealth&) const { return false; }

private:
    bool mAudioPublishEnabled{false};
};

#endif  // SUPERSONIC_LINK_AUDIO
