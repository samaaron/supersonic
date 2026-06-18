/*
 * LinkAudioBridge.cpp — Link Audio ↔ scsynth bus-pool bridge (real impl).
 *
 * Compiled only when SUPERSONIC_LINK_AUDIO (Ableton Link && scsynth). Owns the
 * publish-side main sink + aux sinks, the receive-side input subscriptions, and
 * the RT-thread drain/publish across Link's channels and scsynth's bus pool.
 * The ableton::LinkAudio instance is borrowed from SuperClockNative by
 * reference; clock sync (tempo/transport/peers) stays there.
 */
#include "native/LinkAudioBridge.h"

#if SUPERSONIC_LINK_AUDIO

#include <ableton/util/FloatIntConversion.hpp>

#include <algorithm>
#include <chrono>
#include <cstdio>

// touch_audio_bus is declared by JuceAudioCallback.h (extern "C" pass-through to
// audio_processor.cpp). Forward-declare here so we don't drag the whole header
// in. drain runs BEFORE process_audio's mBufCounter++ so we call the
// _for_next_block variant to land in scsynth's In.ar visibility window for the
// upcoming block.
extern "C" void touch_audio_bus(uint32_t busIdx);
extern "C" void touch_audio_bus_for_next_block(uint32_t busIdx);

namespace {

// Render an 8-byte Link NodeId/PeerId/ChannelId as 16 lowercase hex.
template <typename Bytes>
inline std::string bytesToHex16(const Bytes& bytes) {
    char buf[17];
    for (size_t k = 0; k < bytes.size(); ++k) {
        std::snprintf(buf + 2 * k, 3, "%02x", bytes[k]);
    }
    return std::string(buf, 16);
}

// Use the caller-supplied audio-framework timestamp if available; otherwise
// fall back to link.clock().micros() (jittery; testing only).
inline std::chrono::microseconds hostMicrosOrNow(
    uint64_t hostMicros, const ableton::LinkAudio& link) {
    return hostMicros > 0
        ? std::chrono::microseconds{static_cast<int64_t>(hostMicros)}
        : link.clock().micros();
}

// Interleave two float channels (R nullable for mono) into one int16 buffer
// with Link's saturating float→int16 conversion.
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

}  // namespace

// ─── Publish gate + channel discovery ────────────────────────────────────

void LinkAudioBridge::setPublishEnabled(bool publish, bool linkEnabled) {
    if (mAudioPublishEnabled == publish) return;
    mAudioPublishEnabled = publish;

    // Reflect immediately if the Link mesh is already up; otherwise the next
    // ensureMainSink (on setLinkVisibility(non-Off)) creates the sink.
    // LinkAudio stays enabled in both directions — we keep observing peers'
    // channels even after we stop publishing.
    if (!linkEnabled) return;
    if (publish) {
        std::lock_guard<std::mutex> lk(mSinkMutex);
        mSink.emplace(mLink, mChannelNameCache, kSinkMaxSamples);
    } else {
        // Aux sinks share the publish-side substrate — clear them when publish
        // goes off; the main sink stops broadcasting too.
        {
            std::lock_guard<std::mutex> auxLk(mAuxSinksMutex);
            mAuxSinks.clear();
            mAuxSinkCount.store(0, std::memory_order_relaxed);
        }
        {
            std::lock_guard<std::mutex> lk(mSinkMutex);
            if (mSink) mSink.reset();
        }
    }
}

std::vector<SuperClock::LinkAudioChannel> LinkAudioBridge::listChannels() const {
    std::vector<SuperClock::LinkAudioChannel> out;
    auto channels = mLink.channels();
    out.reserve(channels.size());
    for (auto& c : channels) {
        out.push_back({bytesToHex16(c.id), c.name,
                       bytesToHex16(c.peerId), c.peerName});
    }
    return out;
}

// ─── Input subscriptions ──────────────────────────────────────────────────

bool LinkAudioBridge::addInput(const char* peerName, const char* channelName,
                               uint32_t busIdx) {
    if (!peerName || !channelName) return false;

    auto channels = mLink.channels();
    auto match = std::find_if(channels.begin(), channels.end(),
        [&](const auto& c) {
            return c.peerName == peerName && c.name == channelName;
        });
    if (match == channels.end()) return false;
    const ableton::ChannelId newChannelId = match->id;

    // First pass: re-arm short-circuit (matching channelId reuses the existing
    // renderer + counters). Subscribe stays outside the lock — Link's
    // source-callback interacts with its own threading.
    {
        std::lock_guard<std::mutex> lk(mInputSubMutex);
        InputSubscription* existing = nullptr;
        for (auto& s : mInputSubs) {
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
    InputSubscription sub;
    sub.busIdx      = busIdx;
    sub.peerName    = peerName;
    sub.channelName = channelName;
    sub.channelId   = newChannelId;
    sub.renderer = std::make_unique<
        supersonic_link::LinkAudioInputRenderer<ableton::LinkAudio>>(mLink);
    sub.renderer->subscribe(newChannelId);

    std::lock_guard<std::mutex> lk(mInputSubMutex);
    // Re-validate under the lock; state may have shifted while we were building.
    InputSubscription* replaceSlot = nullptr;
    for (auto& s : mInputSubs) {
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
    mInputSubs.push_back(std::move(sub));
    mInputSubCount.store(mInputSubs.size(), std::memory_order_relaxed);
    return true;
}

void LinkAudioBridge::removeInput(const char* peerName, const char* channelName) {
    if (!peerName || !channelName) return;
    std::lock_guard<std::mutex> lk(mInputSubMutex);
    auto& v = mInputSubs;
    v.erase(std::remove_if(v.begin(), v.end(),
            [&](const auto& s) {
                return s.peerName == peerName && s.channelName == channelName;
            }),
            v.end());
    mInputSubCount.store(v.size(), std::memory_order_relaxed);
}

void LinkAudioBridge::clearInputs() {
    std::lock_guard<std::mutex> lk(mInputSubMutex);
    mInputSubs.clear();
    mInputSubCount.store(0, std::memory_order_relaxed);
}

bool LinkAudioBridge::setInputLatencySeconds(const char* peerName,
                                             const char* channelName,
                                             double seconds) {
    if (!peerName || !channelName) return false;
    if (!(seconds >= 0.0) ||
        seconds > SuperClock::kMaxLinkAudioInputLatencySeconds) {
        return false;
    }
    std::lock_guard<std::mutex> lk(mInputSubMutex);
    for (auto& sub : mInputSubs) {
        if (sub.peerName == peerName && sub.channelName == channelName) {
            sub.renderer->setLatencySeconds(seconds);
            return true;
        }
    }
    return false;
}

std::vector<SuperClock::LinkAudioInputStatus> LinkAudioBridge::listInputs() const {
    std::vector<SuperClock::LinkAudioInputStatus> out;
    std::lock_guard<std::mutex> lk(mInputSubMutex);
    out.reserve(mInputSubs.size());
    for (const auto& sub : mInputSubs) {
        SuperClock::LinkAudioInputStatus s;
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
            s.state = SuperClock::LinkAudioConnectionState::Connecting;
        } else if (s.bufferedSeconds < kMinHealthyBufferSeconds) {
            s.state = SuperClock::LinkAudioConnectionState::Dropout;
        } else {
            s.state = SuperClock::LinkAudioConnectionState::Connected;
        }
        out.push_back(std::move(s));
    }
    return out;
}

// ─── Auxiliary sinks ──────────────────────────────────────────────────────

bool LinkAudioBridge::addSink(const char* name, uint32_t busIdx,
                              uint32_t numChannels) {
    if (!name || numChannels == 0 || numChannels > 2) return false;
    // Construct outside the lock — LinkAudioSink ctor isn't RT-safe but we're
    // on the app thread, and we don't want it inside the lock.
    ActiveSink entry{
        std::string(name),
        busIdx,
        numChannels,
        ableton::LinkAudioSink(mLink, std::string(name), kSinkMaxSamples)
    };
    std::lock_guard<std::mutex> lk(mAuxSinksMutex);
    for (auto& as : mAuxSinks) {
        if (as.name == name) {
            as = std::move(entry);
            return true;
        }
    }
    mAuxSinks.push_back(std::move(entry));
    mAuxSinkCount.store(mAuxSinks.size(), std::memory_order_relaxed);
    return true;
}

void LinkAudioBridge::removeSink(const char* name) {
    if (!name) return;
    std::lock_guard<std::mutex> lk(mAuxSinksMutex);
    auto& v = mAuxSinks;
    v.erase(std::remove_if(v.begin(), v.end(),
            [&](const auto& as) { return as.name == name; }),
            v.end());
    mAuxSinkCount.store(v.size(), std::memory_order_relaxed);
}

std::vector<SuperClock::ActiveSinkInfo> LinkAudioBridge::listSinks() const {
    std::vector<SuperClock::ActiveSinkInfo> out;
    std::lock_guard<std::mutex> lk(mAuxSinksMutex);
    out.reserve(mAuxSinks.size());
    for (const auto& as : mAuxSinks) {
        out.push_back({as.name, as.busIdx, as.numChannels, as.hasSubscriber});
    }
    return out;
}

// ─── RT-thread: publish + drain (RT-safe) ─────────────────────────────────

void LinkAudioBridge::publishAuxSinks(const float* busPool, uint32_t blockSize,
                                      uint32_t numBuses, uint32_t sampleRate,
                                      uint64_t hostMicrosForBufferBegin,
                                      double quantum) {
    if (!busPool) return;
    // Fast path: skip the mutex entirely when there are no aux sinks.
    if (mAuxSinkCount.load(std::memory_order_relaxed) == 0) return;
    // try_lock keeps the audio thread RT-friendly: skip block on contention,
    // next one recovers.
    std::unique_lock<std::mutex> lk(mAuxSinksMutex, std::try_to_lock);
    if (!lk.owns_lock()) return;
    if (mAuxSinks.empty()) return;

    auto sessionState = mLink.captureAudioSessionState();
    const auto hostMicros = hostMicrosOrNow(hostMicrosForBufferBegin, mLink);
    const double beatsAtBegin = sessionState.beatAtTime(hostMicros, quantum);

    for (auto& as : mAuxSinks) {
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
}

void LinkAudioBridge::drainInputsToBuses(float* busPool, uint32_t blockSize,
                                         uint32_t numBuses, uint32_t sampleRate,
                                         uint64_t hostMicrosForBufferBegin) {
    if (!busPool) return;
    // Lock-free fast path: no subscriptions, no mutex acquisition.
    if (mInputSubCount.load(std::memory_order_relaxed) == 0) return;
    // try_lock: skip the block if an OSC-driven add/remove/clear is
    // mid-mutation. Next block recovers.
    std::unique_lock<std::mutex> lk(mInputSubMutex, std::try_to_lock);
    if (!lk.owns_lock()) return;
    if (mInputSubs.empty()) return;

    // Each subscription renders stereo into (busIdx, busIdx+1). Mono sources
    // are mirrored to both buses by the renderer.
    double* const scratchL = mDrainScratchL;
    double* const scratchR = mDrainScratchR;
    // Skip the whole drain if blockSize exceeds the scratch — partial fills
    // would leave the bus's tail silent while touch_audio_bus marks it fresh,
    // which In.ar consumers can't distinguish from real audio.
    if (blockSize > kDrainScratchFrames) return;
    const size_t framesToRender = blockSize;

    auto sessionState = mLink.captureAudioSessionState();
    const auto hostTime = hostMicrosOrNow(hostMicrosForBufferBegin, mLink);

    for (auto& sub : mInputSubs) {
        if (sub.busIdx + 1 >= numBuses) continue;
        const size_t framesFilled = sub.renderer->receive(
            scratchL, scratchR, framesToRender, sessionState,
            static_cast<double>(sampleRate), hostTime, /*quantum=*/4.0);

        // Couldn't fill the whole block → the queue ran dry: an underrun.
        if (framesFilled < framesToRender)
            mLinkAudioUnderruns.fetch_add(1, std::memory_order_relaxed);

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
}

bool LinkAudioBridge::publishAudioBlock(const float* leftChannel,
                                        const float* rightChannel,
                                        size_t numFrames, uint32_t sampleRate,
                                        uint64_t hostMicrosForBufferBegin,
                                        double quantum) {
    // try_lock: skip the publish if setLinkVisibility / setPublishEnabled is
    // mid-reset of mSink.
    std::unique_lock<std::mutex> lk(mSinkMutex, std::try_to_lock);
    if (!lk.owns_lock()) return false;
    if (!mSink) return false;
    ableton::LinkAudioSink::BufferHandle buf(*mSink);
    if (!buf) return false;

    const size_t numChannels = rightChannel ? 2u : 1u;
    if (numFrames * numChannels > buf.maxNumSamples) return false;

    interleaveFloatToInt16(leftChannel, rightChannel, buf.samples, numFrames);

    const auto hostMicros = hostMicrosOrNow(hostMicrosForBufferBegin, mLink);
    auto st = mLink.captureAudioSessionState();
    const double beatsAtBegin = st.beatAtTime(hostMicros, quantum);

    return buf.commit(st, beatsAtBegin, quantum,
                      numFrames, numChannels, sampleRate);
}

// ─── Visibility-change support ────────────────────────────────────────────

void LinkAudioBridge::resetForVisibilityChange() {
    {
        std::lock_guard<std::mutex> lk(mInputSubMutex);
        mInputSubs.clear();
        mInputSubCount.store(0, std::memory_order_relaxed);
    }
    {
        std::lock_guard<std::mutex> auxLk(mAuxSinksMutex);
        mAuxSinks.clear();
        mAuxSinkCount.store(0, std::memory_order_relaxed);
    }
    {
        std::lock_guard<std::mutex> lk(mSinkMutex);
        if (mSink) mSink.reset();
    }
}

void LinkAudioBridge::ensureMainSink() {
    if (!mAudioPublishEnabled) return;
    std::lock_guard<std::mutex> lk(mSinkMutex);
    mSink.emplace(mLink, mChannelNameCache, kSinkMaxSamples);
}

// ─── Metrics ──────────────────────────────────────────────────────────────

bool LinkAudioBridge::tryReadSinkCount(uint32_t& outCount) const {
    std::unique_lock<std::mutex> lk(mAuxSinksMutex, std::try_to_lock);
    if (!lk.owns_lock()) return false;
    outCount = static_cast<uint32_t>(mAuxSinks.size());
    return true;
}

bool LinkAudioBridge::tryReadInputHealth(InputHealth& out) const {
    std::unique_lock<std::mutex> lk(mInputSubMutex, std::try_to_lock);
    if (!lk.owns_lock()) return false;
    uint32_t inCh = 0, rate = 0;
    int32_t  drift = 0;
    float    bufMs = 0.0f;
    for (auto& sub : mInputSubs) {
        inCh += sub.renderer->lastNumChannels();
        rate  = sub.renderer->lastSampleRate();
        drift = sub.renderer->lastDriftPpm();
        bufMs = std::max(bufMs, sub.renderer->bufferedSeconds() * 1000.0f);
    }
    out.inChannels = inCh;
    out.streamRate = rate;
    out.driftPpm   = drift;
    out.bufferedMs = static_cast<uint32_t>(bufMs);
    return true;
}

#endif  // SUPERSONIC_LINK_AUDIO
