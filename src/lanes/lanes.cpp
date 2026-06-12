/*
    SuperSonic - SuperCollider AudioWorklet WebAssembly port
    Copyright (c) 2025 Sam Aaron

    Based on SuperCollider by James McCartney and community
    GPL v3 or later
*/

/*
 * lanes.cpp — the engine boundary, implemented over the engine's existing
 * transport state. See lanes.h for the contract.
 *
 * Ingress and the NRT producer delegate to RingBufferWriter; the egress
 * drains delegate to ss_drain_ring (ring_drain.h, also used by the native
 * RingReader thread); the tick is process_audio.
 */
#include "lanes.h"
#include "lanes_internal.h"
#include "ring_drain.h"

#include <atomic>
#include <cstring>

#include "../audio_processor.h"          // arena globals + process_audio + accessors
#include "../shared_memory.h"            // layout, ControlPointers, EgressRoute
#include "../workers/RingBufferWriter.h" // the single ring writer

// ── internal state ──────────────────────────────────────────────────────────

// NRT egress producer lock. Producers on control threads serialise their
// [route][osc] framing through here; this is the process-wide lock for the
// NRT-out lane (the engine's OscEgress delegates to ss_egress_nrt_write).
static std::atomic<int32_t> g_nrt_egress_lock{0};

// Largest NRT egress frame the producer accepts (replies/notifies are small).
static constexpr uint32_t kNrtEgressMax = 4096;

// Per-lane consumer state (single consumer per lane, by contract). Process
// lifetime; init_memory() resets it alongside the ring sequence counters via
// ss_lanes_reset_drains.
static SsDrainState g_rt_drain_state;
static SsDrainState g_nrt_drain_state;

extern "C" {

void ss_lanes_reset_drains(void) {
    g_rt_drain_state.lastSeq  = -1;
    g_nrt_drain_state.lastSeq = -1;
}

// ── Ingress ─────────────────────────────────────────────────────────────────

bool ss_ingress_write(const uint8_t* osc, uint32_t len, uint32_t source_id) {
    if (!memory_initialized || !shared_memory || !control || !osc || len == 0)
        return false;
    return RingBufferWriter::write(
        shared_memory + IN_BUFFER_START, IN_BUFFER_SIZE,
        &control->in_head, &control->in_tail,
        &control->in_sequence, &control->in_write_lock,
        osc, len, source_id);
}

// ── Egress ──────────────────────────────────────────────────────────────────

// Both egress rings carry Message frames whose payload is [route:u32][osc];
// the drains peel the route word before the callback sees the OSC bytes.
static uint32_t drain_egress_ring(uint8_t* buffer, uint32_t size,
                                  std::atomic<int32_t>* head,
                                  std::atomic<int32_t>* tail,
                                  SsDrainState& st,
                                  const SsDrainMetrics& m,
                                  SsEgressFn fn, void* ctx,
                                  uint32_t max_frames) {
    return ss_drain_ring(buffer, size, head, tail, st, m, max_frames,
        [fn, ctx](uint32_t sourceId, const uint8_t* payload, uint32_t n, uint32_t seq) {
            if (n >= EGRESS_ROUTE_SIZE) {
                uint32_t route;
                std::memcpy(&route, payload, sizeof(route));
                fn(ctx, sourceId, route,
                   payload + EGRESS_ROUTE_SIZE, n - EGRESS_ROUTE_SIZE, seq);
            }
            return SsDrainVerdict::Consume;
        });
}

uint32_t ss_egress_rt_drain(SsEgressFn fn, void* ctx, uint32_t max_frames) {
    if (!memory_initialized || !shared_memory || !control || !fn) return 0;
    // RT egress traffic counts into the segment-resident metrics so external
    // observers see reply/notification volume and ring health.
    SsDrainMetrics m;
    if (metrics) {
        m.received  = &metrics->osc_in_messages_received;
        m.bytes     = &metrics->osc_in_bytes_received;
        m.corrupted = &metrics->osc_in_corrupted;
        m.seqGaps   = &metrics->messages_sequence_gaps;
    }
    return drain_egress_ring(shared_memory + OUT_BUFFER_START, OUT_BUFFER_SIZE,
                             &control->out_head, &control->out_tail,
                             g_rt_drain_state, m, fn, ctx, max_frames);
}

uint32_t ss_egress_nrt_drain(SsEgressFn fn, void* ctx, uint32_t max_frames) {
    if (!memory_initialized || !shared_memory || !control || !fn) return 0;
    return drain_egress_ring(shared_memory + NRT_OUT_BUFFER_START, NRT_OUT_BUFFER_SIZE,
                             &control->nrt_out_head, &control->nrt_out_tail,
                             g_nrt_drain_state, SsDrainMetrics{}, fn, ctx, max_frames);
}

bool ss_egress_nrt_write(uint32_t route, uint32_t token,
                         const uint8_t* osc, uint32_t len) {
    if (!memory_initialized || !shared_memory || !control || !osc || len == 0)
        return false;
    // Subtraction form: len + EGRESS_ROUTE_SIZE would wrap for len near
    // UINT32_MAX and slip past the bound into the stack memcpy below.
    if (len > kNrtEgressMax - EGRESS_ROUTE_SIZE) return false;

    uint8_t buf[kNrtEgressMax];
    std::memcpy(buf, &route, sizeof(route));
    std::memcpy(buf + sizeof(route), osc, len);
    return RingBufferWriter::write(
        shared_memory + NRT_OUT_BUFFER_START, NRT_OUT_BUFFER_SIZE,
        &control->nrt_out_head, &control->nrt_out_tail,
        &control->nrt_out_sequence, &g_nrt_egress_lock,
        buf, len + EGRESS_ROUTE_SIZE, token);
}

// ── Tick ────────────────────────────────────────────────────────────────────

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
bool ss_tick(double ntp_now, uint32_t out_channels, uint32_t in_channels) {
    return process_audio(ntp_now, out_channels, in_channels);
}

const float* ss_audio_out(void) {
    return reinterpret_cast<const float*>(get_audio_output_bus());
}

float* ss_audio_in(void) {
    return reinterpret_cast<float*>(get_audio_input_bus());
}

uint32_t ss_block_size(void) {
    return static_cast<uint32_t>(get_audio_buffer_samples());
}

// ── Layout ──────────────────────────────────────────────────────────────────

const SsLanesLayout* ss_lanes_layout(void) {
    return reinterpret_cast<const SsLanesLayout*>(get_buffer_layout());
}

void* ss_lanes_base(void) {
    return get_shared_memory_base();
}

}  // extern "C"
