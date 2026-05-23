/* Copyright 2025, Ableton AG, Berlin. All rights reserved.
 *
 * Receive-only LinkAudio renderer. Adapted from upstream's
 * examples/linkaudio/LinkAudioRenderer.hpp — the cubic-interpolation
 * receive algorithm and SPSC-queue wiring are unchanged; the send path
 * and bound LinkAudioSink are removed because we manage sinks via our
 * own aux-sink registry (otherwise upstream would announce a phantom
 * "A Sink" channel per subscription).
 *
 * License inherited from upstream Link: GNU GPL v2 or later.
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#pragma once

#include <ableton/LinkAudio.hpp>
#include <ableton/link_audio/Queue.hpp>
#include <ableton/util/FloatIntConversion.hpp>

#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>

namespace supersonic_link
{

namespace
{

template <typename T>
T cubicInterpolate(const std::array<T, 4>& p, double t)
{
  double a = -0.5 * static_cast<double>(p[0]) + 1.5 * static_cast<double>(p[1])
             - 1.5 * static_cast<double>(p[2]) + 0.5 * static_cast<double>(p[3]);
  double b = static_cast<double>(p[0]) - 2.5 * static_cast<double>(p[1])
             + 2.0 * static_cast<double>(p[2]) - 0.5 * static_cast<double>(p[3]);
  double c = -0.5 * static_cast<double>(p[0]) + 0.5 * static_cast<double>(p[2]);
  auto d = static_cast<double>(p[1]);
  return static_cast<T>(a * t * t * t + b * t * t + c * t + d);
}

template <typename T>
T linearInterpolate(T value, T inMin, T inMax, T outMin, T outMax)
{
  return (value - inMin) * (outMax - outMin) / (inMax - inMin) + outMin;
}

} // namespace

// Receive-only LinkAudio buffer renderer.
//   * Constructor takes only `link` (no sink, no sampleRate-by-reference).
//   * Subscribe to a channel via subscribe(channelId).
//   * Audio thread calls receive(out, numFrames, sessionState, sampleRate,
//     hostTime, quantum) to fill `out` with cubic-interpolated samples
//     from the channel, aligned to beat-time with a 4-beat lookahead.
//   * Buffers arriving over the network at a different sample rate / tempo
//     are automatically re-pitched by frameIncrement = totalFrames /
//     numFrames.
template <typename Link>
class LinkAudioInputRenderer
{
  // Link Audio's commit() validates numChannels ∈ {1, 2} — see
  // LinkAudio.ipp. Mono sources are mirrored to both internal channels
  // at receive time so receive() always produces a stereo pair and
  // consumers always allocate two consecutive buses.
  static constexpr size_t kMaxChannels = 2;
  // Link's Resizer caps every wire chunk at ~576 bytes (RFC 791 floor;
  // see Encoder.hpp). That's ~125 stereo frames / ~250 mono at int16.
  // 512 has ~4× headroom and survives Link raising the cap toward an
  // MTU-sized payload (~350 stereo frames at 1500-byte MTU).
  // Truncating a delivered buffer breaks Link's beat-time continuity.
  static constexpr size_t kMaxFramesPerBuffer = 512;

  struct Buffer
  {
    std::array<double, kMaxFramesPerBuffer> mSamplesL;
    std::array<double, kMaxFramesPerBuffer> mSamplesR;
    ableton::LinkAudioSource::BufferHandle::Info mInfo;
  };
  using Queue = ableton::link_audio::Queue<Buffer>;

public:
  LinkAudioInputRenderer(Link& link)
    : mLink(link)
  {
    // Depth sized for Live's 2 s lookahead ceiling at Link's wire
    // chunk rate (~780/s for stereo 48 kHz → 1560 needed; 2048 gives
    // ~30 % headroom). Per slot = 2 × kMaxFramesPerBuffer × 8 B = 8 KiB
    // → ~16 MiB per subscription. The 50 ms default uses ~40 slots.
    auto queue = Queue(2048, {});
    mpQueueWriter = std::make_shared<typename Queue::Writer>(std::move(queue.writer()));
    mpQueueReader = std::make_shared<typename Queue::Reader>(std::move(queue.reader()));
  }

  ~LinkAudioInputRenderer() { mpSource.reset(); }

  bool hasSource() const { return mpSource != nullptr; }

  // Diagnostics — last observed info from the most-recently-delivered
  // buffer. Zero until a buffer arrives. Thread-safe: written on Link
  // thread (single writer), read on any thread.
  uint32_t lastSampleRate() const {
      return mLastSampleRate.load(std::memory_order_relaxed);
  }
  uint32_t lastNumChannels() const {
      return mLastNumChannels.load(std::memory_order_relaxed);
  }
  // Approximate buffered audio in seconds, updated on each receive() pass.
  // 0 if not currently rendering.
  float bufferedSeconds() const {
      return mBufferedSeconds.load(std::memory_order_relaxed);
  }
  // True if we've received at least one buffer.
  bool everReceived() const {
      return mLastSampleRate.load(std::memory_order_relaxed) > 0;
  }
  // Consumer-side drops (our ring was full at producer push time).
  uint64_t droppedSourceBuffers() const {
      return mDroppedSourceBuffers.load(std::memory_order_relaxed);
  }
  // Upstream/network loss — gaps in BufferHandle::Info::count.
  uint64_t networkGapBuffers() const {
      return mNetworkGapBuffers.load(std::memory_order_relaxed);
  }
  // Raw onSourceBuffer invocation count. Total >> peer publish rate
  // indicates duplicate delivery (multi-interface / retransmits).
  uint64_t totalSourceBufferCalls() const {
      return mTotalSourceBufferCalls.load(std::memory_order_relaxed);
  }
  // Count of invocations whose sequence number equalled the prior
  // call — proves duplicate delivery vs net-new buffers.
  uint64_t duplicateCountCalls() const {
      return mDuplicateCountCalls.load(std::memory_order_relaxed);
  }

  void subscribe(const ableton::ChannelId& channelId)
  {
    mpSource = std::make_unique<ableton::LinkAudioSource>(
      mLink, channelId,
      [this](ableton::LinkAudioSource::BufferHandle bh) { onSourceBuffer(bh); });
  }

  void unsubscribe()
  {
    if (!mpSource) return;
    mpSource.reset();
    // Drain any queued slots so the next subscription starts clean.
    while (mpQueueReader->retainSlot()) {}
    while (mpQueueReader->numRetainedSlots() > 0) mpQueueReader->releaseSlot();
    moLastFrameIdx = std::nullopt;
    moStartReadPos = std::nullopt;
  }

  // Playback lookahead in wall-clock seconds — consumer plays buffers
  // this far behind wall-clock to absorb network jitter / clock drift.
  // Default 0.05 s matches wired-LAN; Live's UI caps at 2.0 s.
  // Stored as bit-packed atomic<uint64_t> for portable lock-free
  // access (atomic<double> falls back to mutex on some 32-bit ARM).
  void setLatencySeconds(double seconds) {
      if (!(seconds >= 0.0)) seconds = 0.0;
      uint64_t bits;
      std::memcpy(&bits, &seconds, sizeof(bits));
      mLatencySecondsBits.store(bits, std::memory_order_relaxed);
  }
  double latencySeconds() const {
      const uint64_t bits = mLatencySecondsBits.load(std::memory_order_relaxed);
      double seconds;
      std::memcpy(&seconds, &bits, sizeof(seconds));
      return seconds;
  }

  // Audio-thread: fill outL[0..numFrames) and outR[0..numFrames) with
  // audio aligned to (hostTime − latencySeconds) on the Link beat-time
  // grid. Always stereo (mono sources are duplicated). Returns frames
  // filled; un-filled tail is left untouched. Returns 0 when the
  // queue is empty or buffers don't span the target beat range.
  size_t receive(double* outL,
                 double* outR,
                 size_t numFrames,
                 typename Link::SessionState sessionState,
                 double sampleRate,
                 std::chrono::microseconds hostTime,
                 double quantum)
  {
    // Drain all available slots into our reader half.
    while (mpQueueReader->retainSlot()) {}

    // Convert wall-clock lookahead → beats at the current tempo, so
    // latency stays stable in seconds regardless of BPM.
    const double bpm = sessionState.tempo();
    const double safeBpm = (bpm >= 1.0) ? bpm : 120.0;
    const double kLatencyInBeats =
      latencySeconds() * safeBpm / 60.0;

    const auto targetBeatsAtBufferBegin =
      sessionState.beatAtTime(hostTime, quantum) - kLatencyInBeats;
    const auto targetBeatsAtBufferEnd =
      sessionState.beatAtTime(
        hostTime + std::chrono::duration_cast<std::chrono::microseconds>(
                     std::chrono::duration<double>(double(numFrames) / sampleRate)),
        quantum) - kLatencyInBeats;

    // Drop too-old slots while we're not actively rendering.
    while (!moStartReadPos && mpQueueReader->numRetainedSlots() > 0)
    {
      if ((*mpQueueReader)[0]->mInfo.endBeats(sessionState, quantum)
          < targetBeatsAtBufferBegin)
      {
        mpQueueReader->releaseSlot();
      }
      else break;
    }

    if (mpQueueReader->numRetainedSlots() == 0)
    {
      moLastFrameIdx = std::nullopt;
      moStartReadPos = std::nullopt;
      return 0;
    }

    // Wait for buffers to catch up to our target.
    if (!moStartReadPos
        && (*mpQueueReader)[0]->mInfo.beginBeats(sessionState, quantum)
             > targetBeatsAtBufferBegin)
    {
      moLastFrameIdx = std::nullopt;
      moStartReadPos = std::nullopt;
      return 0;
    }

    if (!moStartReadPos)
    {
      const auto& info = (*mpQueueReader)[0]->mInfo;
      const auto startBufferBegin = *info.beginBeats(sessionState, quantum);
      const auto startBufferEnd = *info.endBeats(sessionState, quantum);
      moStartReadPos = linearInterpolate(targetBeatsAtBufferBegin,
                                         startBufferBegin, startBufferEnd,
                                         0.0, double(info.numFrames));
      // Prime the cubic-interp cache from the first samples so the
      // initial output frames don't underweight a zero-init cache and
      // click. Without this, only cache[0] gets a real sample on the
      // first advance — cubicInterpolate weights [1] and [2] most so
      // the very first output frame would be near-zero regardless of
      // the actual source amplitude.
      const auto seedL = (*mpQueueReader)[0]->mSamplesL[0];
      const auto seedR = (*mpQueueReader)[0]->mSamplesR[0];
      mReceiverSampleCache[0] = {{seedL, seedL, seedL, seedL}};
      mReceiverSampleCache[1] = {{seedR, seedR, seedR, seedR}};
    }

    const auto startFramePos = *moStartReadPos;

    // Sum total source frames spanning the target beat range.
    auto totalFrames = 0.0;
    auto foundEnd = false;
    for (auto i = 0u; i < mpQueueReader->numRetainedSlots(); ++i)
    {
      const auto& info = (*mpQueueReader)[i]->mInfo;
      const auto bufferBegin = *info.beginBeats(sessionState, quantum);
      const auto bufferEnd = *info.endBeats(sessionState, quantum);
      if (targetBeatsAtBufferEnd >= bufferBegin
          && targetBeatsAtBufferEnd < bufferEnd)
      {
        const auto targetBeatsFrame = linearInterpolate(
          targetBeatsAtBufferEnd, bufferBegin, bufferEnd, 0.0, double(info.numFrames));
        totalFrames += targetBeatsFrame;
        foundEnd = true;
        break;
      }
      totalFrames += double(info.numFrames);
    }

    if (!foundEnd)
    {
      moLastFrameIdx = std::nullopt;
      moStartReadPos = std::nullopt;
      return 0;
    }

    totalFrames -= startFramePos;
    if (totalFrames <= 0.0)
    {
      moLastFrameIdx = std::nullopt;
      moStartReadPos = std::nullopt;
      return 0;
    }

    const auto frameIncrement = totalFrames / double(numFrames);
    auto readPos = startFramePos;

    auto getSample = [&](size_t idx, size_t ch) -> double {
      size_t bufferIdx = 0;
      while (bufferIdx < mpQueueReader->numRetainedSlots())
      {
        auto& cur = *((*mpQueueReader)[bufferIdx]);
        if (idx < cur.mInfo.numFrames) {
          return ch == 0 ? cur.mSamplesL[idx] : cur.mSamplesR[idx];
        }
        idx -= cur.mInfo.numFrames;
        ++bufferIdx;
      }
      return 0.0;
    };

    for (auto frame = 0u; frame < numFrames; ++frame)
    {
      const auto framePos = readPos + frame * frameIncrement;
      const auto frameIdx = static_cast<size_t>(std::floor(framePos));
      const auto t = framePos - std::floor(framePos);

      while (!moLastFrameIdx || (moLastFrameIdx && frameIdx > *moLastFrameIdx))
      {
        // newLast advances per iteration so frameIncrement > 1 reads
        // successive source samples rather than repeating frameIdx-1.
        const size_t newLast =
          moLastFrameIdx ? (*moLastFrameIdx + 1) : frameIdx;
        for (size_t ch = 0; ch < kMaxChannels; ++ch)
        {
          mReceiverSampleCache[ch][3] = mReceiverSampleCache[ch][2];
          mReceiverSampleCache[ch][2] = mReceiverSampleCache[ch][1];
          mReceiverSampleCache[ch][1] = mReceiverSampleCache[ch][0];
          mReceiverSampleCache[ch][0] = (newLast > 0)
            ? getSample(newLast - 1, ch)
            : getSample(0, ch);
        }
        moLastFrameIdx = newLast;
      }

      outL[frame] = cubicInterpolate(mReceiverSampleCache[0], t);
      outR[frame] = cubicInterpolate(mReceiverSampleCache[1], t);

      const auto& currentInfo = (*mpQueueReader)[0]->mInfo;
      if (frameIdx >= currentInfo.numFrames)
      {
        readPos -= double(currentInfo.numFrames);
        moLastFrameIdx = frameIdx - currentInfo.numFrames;
        mpQueueReader->releaseSlot();
      }
    }

    *moStartReadPos = readPos + double(numFrames) * frameIncrement;

    // Buffered-seconds gauge: frames remaining in the current buffer
    // (numFrames - readPos) plus full duration of every queued buffer
    // behind it. moStartReadPos is the read cursor INSIDE the current
    // buffer.
    if (mpQueueReader->numRetainedSlots() > 0) {
      const auto& curInfo = (*mpQueueReader)[0]->mInfo;
      float buffered =
        (float(curInfo.numFrames) - static_cast<float>(*moStartReadPos)) /
        float(curInfo.sampleRate);
      for (size_t i = 1; i < mpQueueReader->numRetainedSlots(); ++i) {
        const auto& info = (*mpQueueReader)[i]->mInfo;
        buffered += float(info.numFrames) / float(info.sampleRate);
      }
      mBufferedSeconds.store(buffered, std::memory_order_relaxed);
    } else {
      mBufferedSeconds.store(0.0f, std::memory_order_relaxed);
    }

    return numFrames;
  }

private:
  void onSourceBuffer(const ableton::LinkAudioSource::BufferHandle bh)
  {
    // Capture the latest peer-side info for diagnostics regardless of
    // whether we successfully retain a queue slot.
    mLastSampleRate.store(bh.info.sampleRate, std::memory_order_relaxed);
    mLastNumChannels.store(static_cast<uint32_t>(bh.info.numChannels),
                           std::memory_order_relaxed);

    // Gap detection on Link's monotonic per-channel count — surfaces
    // upstream loss before reaching this callback.
    mTotalSourceBufferCalls.fetch_add(1, std::memory_order_relaxed);
    const uint64_t curCount  = bh.info.count;
    const uint64_t prevCount = mLastSeenCount.load(std::memory_order_relaxed);
    if (prevCount > 0 && curCount > prevCount + 1) {
        mNetworkGapBuffers.fetch_add(curCount - prevCount - 1,
                                      std::memory_order_relaxed);
    } else if (prevCount > 0 && curCount == prevCount) {
        mDuplicateCountCalls.fetch_add(1, std::memory_order_relaxed);
    }
    mLastSeenCount.store(curCount, std::memory_order_relaxed);

    if (!mpQueueWriter->retainSlot()) {
        mDroppedSourceBuffers.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    auto& buffer = *((*mpQueueWriter)[0]);
    buffer.mInfo = bh.info;
    // Source numChannels is 1 or 2 (Link's commit() rejects anything
    // else). Mono → duplicate to both internal channels so receive()
    // always emits stereo.
    const size_t srcChans = bh.info.numChannels;
    const size_t n = std::min(bh.info.numFrames, buffer.mSamplesL.size());
    // Truncation would break Link's beat-time continuity (next
    // buffer's sessionBeatTime advances by the untruncated count).
    // kMaxFramesPerBuffer above is sized to avoid hitting this.
    buffer.mInfo.numFrames = n;
    for (size_t i = 0; i < n; ++i)
    {
      const double l = ableton::util::int16ToFloat<double>(
        bh.samples[i * srcChans]);
      const double r = (srcChans >= 2)
        ? ableton::util::int16ToFloat<double>(bh.samples[i * srcChans + 1])
        : l;
      buffer.mSamplesL[i] = l;
      buffer.mSamplesR[i] = r;
    }
    mpQueueWriter->releaseSlot();
  }

  Link& mLink;
  std::unique_ptr<ableton::LinkAudioSource> mpSource;
  std::shared_ptr<typename Queue::Writer> mpQueueWriter;
  std::shared_ptr<typename Queue::Reader> mpQueueReader;
  std::array<std::array<double, 4>, kMaxChannels> mReceiverSampleCache = {{{0.0, 0.0, 0.0, 0.0},
                                                                            {0.0, 0.0, 0.0, 0.0}}};
  std::optional<size_t> moLastFrameIdx;
  std::optional<double> moStartReadPos;

  // Diagnostics — updated by the Link-thread callback / audio thread,
  // read by app-thread status queries. Atomic for safe cross-thread reads.
  std::atomic<uint32_t> mLastSampleRate{0};
  std::atomic<uint32_t> mLastNumChannels{0};
  std::atomic<float>    mBufferedSeconds{0.0f};
  std::atomic<uint64_t> mDroppedSourceBuffers{0};
  std::atomic<uint64_t> mNetworkGapBuffers{0};
  std::atomic<uint64_t> mLastSeenCount{0};
  std::atomic<uint64_t> mTotalSourceBufferCalls{0};
  std::atomic<uint64_t> mDuplicateCountCalls{0};
  // Wall-clock playback lookahead. 50 ms default sized for wired-LAN
  // jitter; configurable up to Live's 2 s ceiling via setLatencySeconds.
  // Stored as bit-packed uint64_t for portable lock-free atomic access
  // (atomic<double> falls back to mutex on some 32-bit ARM targets).
  // Initial value below = bit pattern for 0.05.
  std::atomic<uint64_t> mLatencySecondsBits{[](){
      double d = 0.05;
      uint64_t bits;
      std::memcpy(&bits, &d, sizeof(bits));
      return bits;
  }()};
};

} // namespace supersonic_link
