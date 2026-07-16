//  Scope stream — shared protocol header.
//
//  Fixed-layout single-producer / single-consumer ring of interleaved float
//  audio, one per scope slot, written per-block by the ScopeOut2 UGen and read
//  by visualisers (Sonic Pi's scopes, JS getScope, tests). Same protocol as
//  shm_audio_buffer (monotonic 64-bit write cursor, lossless catch-up reads),
//  with two scope-specific additions:
//
//   - base_engine_frames anchors the slot-local cursor to the engine's global
//     sample position, so a reader can combine it with the sample clock
//     region (shared_memory.h SAMPLE_CLOCK_*) and its own wall clock to ask "which
//     sample is coming out of the speaker right now" — visualisations align to
//     what the listener hears, not to when DSP ran. See
//     docs/scope-streams-sample-clock.md.
//
//   - the ring is sized by SHM_SCOPE_RING_FRAMES (memory_profile.h), decoupled
//     from the capture taps' much larger SUPERSONIC_SHM_AUDIO_FRAMES.
//
//  Copyright (C) 2026 SuperSonic contributors.
//  Dual-licensed under MIT and GPLv3-or-later, at the user's option.

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>

#include "../../memory_profile.h"

namespace detail_shm_scope {

inline constexpr uint32_t SHM_SCOPE_STREAM_CHANNELS = 2;  // ScopeOut2 writes stereo

// Frames a reader must stay behind the ring's oldest edge: a hardware
// callback appends several engine blocks back-to-back (up to
// SUPERSONIC_MAX_BLOCK_SIZE frames each), so a one-block margin is not
// enough. copy_window clamps this to a quarter of small (embedded) rings.
// js/supersonic.js getScope applies the same formula — keep them in step.
inline constexpr uint32_t SHM_SCOPE_READ_MARGIN_FRAMES = 2048;

struct alignas(16) shm_scope_stream {
    // 0 = free, 1 = active. Claim/release is owner-guarded in SC_World.cpp
    // (a superseded unit's late dtor must not stomp a re-claimed slot).
    std::atomic<uint32_t> state;
    uint32_t              channels;
    uint32_t              capacity_frames;
    uint32_t              _pad0;

    // Total frames written since activation. Reader subtracts
    // capacity_frames for the oldest still-readable frame.
    std::atomic<uint64_t> write_position;

    // Engine sample position of frame 0 of this stream: slot-local cursor c
    // is engine frame base_engine_frames + c. Set on the first write after
    // activation (atomic so cross-process readers see a torn-free value).
    std::atomic<uint64_t> base_engine_frames;

    // Interleaved float ring [ch0_f0, ch1_f0, ch0_f1, ...], wraps modulo
    // capacity_frames.
    float data[SHM_SCOPE_RING_FRAMES * SHM_SCOPE_STREAM_CHANNELS];
};

static_assert(std::is_trivially_destructible<shm_scope_stream>::value,
              "shm_scope_stream must be trivially destructible (lives in shm)");
static_assert(offsetof(shm_scope_stream, data) == 32,
              "shm_scope_stream header must be 32 bytes (data 16-aligned)");

inline constexpr uint32_t SHM_SCOPE_STREAM_HEADER_SIZE = 32;
inline constexpr uint32_t SHM_SCOPE_STREAM_SLOT_SIZE = sizeof(shm_scope_stream);

// ──── Producer side ─────────────────────────────────────────────────────
//
// Slot claim/ownership lives in SC_World.cpp; this class only formats the
// slot and appends blocks. Real-time safe: no allocation, lock-free.

class shm_scope_stream_writer {
public:
    shm_scope_stream_writer() = default;
    explicit shm_scope_stream_writer(shm_scope_stream* slot) : _slot(slot) {}

    bool valid() const { return _slot != nullptr; }
    shm_scope_stream* slot() const { return _slot; }

    void activate(uint32_t channels) {
        if (!_slot) return;
        if (channels == 0 || channels > SHM_SCOPE_STREAM_CHANNELS)
            channels = SHM_SCOPE_STREAM_CHANNELS;
        _slot->channels = channels;
        _slot->capacity_frames = SHM_SCOPE_RING_FRAMES;
        _slot->write_position.store(0, std::memory_order_relaxed);
        _slot->base_engine_frames.store(0, std::memory_order_relaxed);
        _slot->state.store(1, std::memory_order_release);
    }

    // Append one block. engine_frames is the engine sample position of
    // channel_data[c][0]; on the first write it anchors the stream, and on
    // every later write it heals discontinuities (a paused node group skips
    // blocks — the cursor jumps forward so the time mapping stays exact; the
    // skipped ring region keeps stale data, acceptable for visualisation).
    void write(const float* const* channel_data, uint32_t num_frames,
               uint64_t engine_frames) {
        if (!_slot || num_frames == 0) return;
        uint64_t pos = _slot->write_position.load(std::memory_order_relaxed);
        if (pos == 0) {
            _slot->base_engine_frames.store(engine_frames,
                                            std::memory_order_relaxed);
        } else {
            const uint64_t base =
                _slot->base_engine_frames.load(std::memory_order_relaxed);
            if (engine_frames > base && engine_frames - base != pos)
                pos = engine_frames - base;
        }
        // Untrusted runtime values re-read from the segment (a corrupt or
        // hostile mapper can rewrite them under a live writer): clamp so the
        // audio thread can never divide by zero or index past the inline ring.
        uint32_t cap = _slot->capacity_frames;
        if (cap == 0 || cap > SHM_SCOPE_RING_FRAMES)
            cap = SHM_SCOPE_RING_FRAMES;
        uint32_t channels = _slot->channels;
        if (channels == 0 || channels > SHM_SCOPE_STREAM_CHANNELS)
            channels = SHM_SCOPE_STREAM_CHANNELS;
        uint32_t at = static_cast<uint32_t>(pos % cap);
        uint32_t first = (cap - at < num_frames) ? (cap - at) : num_frames;
        float* dst = _slot->data + static_cast<size_t>(at) * channels;
        for (uint32_t f = 0; f < first; ++f)
            for (uint32_t c = 0; c < channels; ++c)
                dst[f * channels + c] = channel_data[c][f];
        if (first < num_frames) {
            const uint32_t wrap = num_frames - first;
            dst = _slot->data;
            for (uint32_t f = 0; f < wrap; ++f)
                for (uint32_t c = 0; c < channels; ++c)
                    dst[f * channels + c] = channel_data[c][first + f];
        }
        _slot->write_position.store(pos + num_frames, std::memory_order_release);
    }

private:
    shm_scope_stream* _slot = nullptr;
};

// ──── Consumer side ─────────────────────────────────────────────────────

class shm_scope_stream_reader {
public:
    shm_scope_stream_reader() = default;
    // keepalive pins the mapping the slot pointer reaches into (see
    // server_shared_memory_client); reader copies survive the client that
    // minted them. Engine-side/test readers pass nothing.
    explicit shm_scope_stream_reader(shm_scope_stream* slot,
                                     std::shared_ptr<const void> keepalive = nullptr)
        : _slot(slot), _keepalive(std::move(keepalive)) {}

    bool valid() const {
        return _slot && _slot->state.load(std::memory_order_acquire) == 1;
    }
    // channels/capacity are untrusted runtime values re-read from the
    // segment: clamp them so a corrupt or hostile slot can never push
    // copy_window's indices outside the inline data array.
    uint32_t channels() const {
        if (!_slot) return 0;
        const uint32_t ch = _slot->channels;
        return (ch == 0 || ch > SHM_SCOPE_STREAM_CHANNELS)
            ? SHM_SCOPE_STREAM_CHANNELS : ch;
    }
    uint32_t capacity_frames() const {
        if (!_slot) return 0;
        const uint32_t cap = _slot->capacity_frames;
        return (cap == 0 || cap > SHM_SCOPE_RING_FRAMES)
            ? SHM_SCOPE_RING_FRAMES : cap;
    }
    uint64_t write_position() const {
        return _slot ? _slot->write_position.load(std::memory_order_acquire) : 0;
    }
    uint64_t base_engine_frames() const {
        return _slot ? _slot->base_engine_frames.load(std::memory_order_relaxed) : 0;
    }

    // Copy the window of `frames` frames ENDING at slot-local cursor
    // `end_cursor` into `out` (interleaved). The window is clamped to what
    // the ring still holds; frames older than that (or beyond the write
    // position) are zero-filled, so `out` is always fully written and stays
    // window-aligned. Returns the number of real (non-fill) frames.
    //
    // `out` MUST hold frames * SHM_SCOPE_STREAM_CHANNELS floats: the slot's
    // channel count is re-read (and clamped) here, so sizing the buffer from
    // a separate earlier channels() read races a slot re-activation. The
    // stride actually written is reported via `used_channels`.
    uint32_t copy_window(uint64_t end_cursor, uint32_t frames, float* out,
                         uint32_t* used_channels = nullptr) const {
        const uint32_t channels = _slot ? this->channels() : SHM_SCOPE_STREAM_CHANNELS;
        if (used_channels) *used_channels = channels;
        if (frames == 0) return 0;
        if (!_slot) {
            // Contract: out is always fully written — a null/expired slot
            // yields silence, not the buffer's prior contents.
            std::memset(out, 0,
                        static_cast<size_t>(frames) * channels * sizeof(float));
            return 0;
        }
        const uint32_t cap = capacity_frames();       // sanitized
        const uint64_t writer = _slot->write_position.load(std::memory_order_acquire);
        if (end_cursor > writer) end_cursor = writer;

        uint64_t start = (end_cursor > frames) ? end_cursor - frames : 0;
        // Oldest frame the ring still holds, plus a margin covering the
        // largest burst a writer appends between reader loads (a hardware
        // callback renders several engine blocks back-to-back). Clamped for
        // small embedded rings.
        const uint32_t margin =
            (cap / 4 < SHM_SCOPE_READ_MARGIN_FRAMES) ? cap / 4
                                                     : SHM_SCOPE_READ_MARGIN_FRAMES;
        const uint64_t oldest = (writer > cap) ? writer - cap + margin : 0;
        if (start < oldest) start = oldest;
        if (start > end_cursor) start = end_cursor;

        const uint32_t real = static_cast<uint32_t>(end_cursor - start);
        const uint32_t fill = frames - real;
        std::memset(out, 0, static_cast<size_t>(fill) * channels * sizeof(float));
        if (real == 0) return 0;

        float* dst = out + static_cast<size_t>(fill) * channels;
        uint32_t at = static_cast<uint32_t>(start % cap);
        uint32_t first = (cap - at < real) ? (cap - at) : real;
        std::memcpy(dst, _slot->data + static_cast<size_t>(at) * channels,
                    static_cast<size_t>(first) * channels * sizeof(float));
        if (first < real)
            std::memcpy(dst + static_cast<size_t>(first) * channels, _slot->data,
                        static_cast<size_t>(real - first) * channels * sizeof(float));
        return real;
    }

private:
    shm_scope_stream* _slot = nullptr;
    std::shared_ptr<const void> _keepalive;
};

} // namespace detail_shm_scope

using detail_shm_scope::shm_scope_stream;
using detail_shm_scope::shm_scope_stream_writer;
using detail_shm_scope::shm_scope_stream_reader;
using detail_shm_scope::SHM_SCOPE_STREAM_CHANNELS;
using detail_shm_scope::SHM_SCOPE_STREAM_HEADER_SIZE;
using detail_shm_scope::SHM_SCOPE_STREAM_SLOT_SIZE;

// Engine sample position at the start of the block currently being rendered.
// Advanced by SuperClock::publishSampleClock / advanceEngineFrames; ScopeOut2
// passes it to write() to anchor and heal each stream's time mapping. On the
// WASM worklet nothing advances it yet, so web streams anchor at 0 and the
// paused-group heal is inert there.
extern std::atomic<uint64_t> g_engine_frames;
