/*
    SuperSonic - SuperCollider AudioWorklet WebAssembly port
    Copyright (c) 2025 Sam Aaron

    Based on SuperCollider by James McCartney and community
    GPL v3 or later
*/

/*
 * lanes.h — the SuperSonic engine boundary.
 *
 * The lowest-common-denominator C API between the engine core and every
 * host: one ingress lane (OSC in), two egress lanes (RT and NRT OSC out),
 * the per-block tick, and the self-describing arena layout for hosts that
 * address the rings directly.
 *
 * Pure C surface: no threads, no IO, no allocation, no JUCE, no
 * Emscripten. Hosts own their audio loop, their wakeups, and their
 * lifecycle; the engine owns everything behind this header.
 *
 * The engine is process-singleton: there is no engine handle, matching the
 * engine's global arena/SuperClock/ingress state.
 *
 * Wire format (shared_memory.h): every ring frame is a 16-byte Message
 * header {magic, length, sequence, sourceId} followed by the payload.
 * Egress payloads carry a leading route word (EgressRoute) before the OSC
 * bytes.
 *
 * ── Minimal host ───────────────────────────────────────────────────────────
 * A self-driven host (browser worklet, embedded, the freestanding CI build)
 * drives the engine through just: ss_init() once, then per block
 * ss_ingress_write() → ss_tick() → read ss_audio_out() → ss_egress_rt_drain().
 * Beyond the ABI calls it must supply a little link-time glue the engine expects
 * a host to define:
 *   - g_external_shared_memory: set null unless pointing the arena at an external
 *     segment (native cross-process shm); a self-driven host leaves it null and
 *     the engine uses its own ring_buffer_storage.
 *   - the SuperClock composition root: link SuperClockNative.cpp + TimeSource.cpp
 *     + MidiTimelines.cpp (a self-driven host defines SUPERSONIC_WORKLET_CLOCK to
 *     get the SAB-formula clock; the LinkSession/LinkAudioBridge headers are
 *     inline no-ops without SUPERSONIC_LINK, so no extra TU). Leave
 *     g_active_superclock null and the engine's clock/MIDI paths stay inert.
 * test/freestanding/freestanding_main.cpp is the reference minimal host: it is
 * the whole contract in ~60 lines, exercised in CI.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Ingress ───────────────────────────────────────────────────────────────
 * Write one complete OSC message or #bundle (wire format) into the IN
 * ring. Callable from ANY thread including the audio thread itself —
 * multi-producer, serialised by an UNBOUNDED spinlock (RingBufferWriter).
 * Holders only memcpy small frames, so the lock is normally held for
 * nanoseconds — but a writer preempted mid-write stalls every other
 * producer, audio thread included, until it is rescheduled. Returns false
 * when the ring is full (backpressure: the caller decides whether to
 * drop, retry, or count it).
 *
 * source_id is an opaque writer/origin token carried in the Message header
 * and surfaced on egress for reply routing. Web: 0 = main thread, 1+ =
 * workers. Native: the transport origin token (0 = in-process embedder).
 *
 * Messages written on the audio thread before ss_tick() are drained by
 * that same tick, subject to the per-block drain bound — so a co-resident
 * component running in the host's audio callback can emit OSC pre-tick and
 * have it performed in the same block.
 */
bool ss_ingress_write(const uint8_t* osc, uint32_t len, uint32_t source_id);

/* ── Egress ────────────────────────────────────────────────────────────────
 * Two single-consumer rings out of the engine:
 *
 *   RT  — written only inside ss_tick() on the audio thread (lock-free):
 *         replies to drained messages, /tr & node notifications, debug
 *         lines framed as /supersonic/debug.
 *   NRT — written by engine control threads (sample loader, MIDI, Link,
 *         …) under the egress lock: async command replies, broadcasts.
 *
 * Each ring must have exactly one draining thread (they may be the same
 * thread — the native NRT gateway drains both). The callback receives one
 * frame at a time; route is an EgressRoute value, source_id the origin
 * token the reply targets, seq the ring sequence number (gap detection is
 * the caller's choice). Payload points into the ring itself (frames are
 * contiguous by wire invariant) and is valid only for the duration of the
 * call.
 *
 * Returns the number of frames delivered. max_frames == 0 means drain
 * everything available.
 */
typedef void (*SsEgressFn)(void* ctx, uint32_t source_id, uint32_t route,
                           const uint8_t* osc, uint32_t len, uint32_t seq);

uint32_t ss_egress_rt_drain(SsEgressFn fn, void* ctx, uint32_t max_frames);
uint32_t ss_egress_nrt_drain(SsEgressFn fn, void* ctx, uint32_t max_frames);

/* The NRT egress *producer* (ss_egress_nrt_write) is engine-internal, not part
 * of this host boundary — hosts only drain egress. See lanes_internal.h. */

/* ── Tick ──────────────────────────────────────────────────────────────────
 * The per-block entry point. Audio thread/task only. One call:
 *   1. drains the ingress lane (bounded per block), classifying through
 *      OscIngress — immediate execution (replies written to the RT egress
 *      lane as messages are performed), or the bundle scheduler;
 *   2. fires scheduled bundles due in this block (sub-sample offsets);
 *   3. runs the DSP graph for one block;
 *   4. flushes node notifications to the RT egress lane.
 *
 * ntp_now: on native builds, the NTP time (seconds since 1900) of this
 * block's first sample, used as-is. On WASM the exported ss_tick receives
 * AudioContext time instead and converts to NTP internally via SuperClock
 * (process_audio's __EMSCRIPTEN__ branch) — the argument's meaning is
 * per-platform. Hosts without a wall clock can pass base NTP + elapsed
 * samples / sample rate.
 *
 * Input audio: write into ss_audio_in() before the call. Output audio:
 * read ss_audio_out() after it — ss_block_size() frames per channel,
 * channel-major, float. Sample-format conversion is host code.
 *
 * Returns false only on fatal engine error (host should stop calling).
 */
bool ss_tick(double ntp_now, uint32_t out_channels, uint32_t in_channels);

const float* ss_audio_out(void);   /* rendered block, channel-major     */
float*       ss_audio_in(void);    /* input bus region, fill before tick */
uint32_t     ss_block_size(void);  /* frames per block (web: 128)        */

/* ── Init ──────────────────────────────────────────────────────────────────
 * Bring the engine up: configure the World, lay out the arena (rings, control,
 * metrics, node-tree, scope), allocate the RT-safe heap, and create the DSP
 * graph. Call once before the first ss_tick.
 *
 * Hosts that hold their options as a struct (native, embedded) call ss_init.
 * The web fills the arena's options block over its SharedArrayBuffer and calls
 * the init_memory export directly — it writes across the JS/wasm boundary, so
 * it stays offset-based. ss_init writes the WHOLE positional options block by
 * named field, so struct-based hosts never hand-write magic indices at boot
 * (the off-by-one that silently created a stray shm segment lived in those
 * hand-written index sites). Runtime single-field updates on a rebuilt World
 * (device switch / channel change in SupersonicEngine) still poke individual
 * indices directly — those sites are not yet funnelled through here.
 *
 * NRT/self-driven is implied — real-time off, memory-locking off, direct memory
 * access — so only the tunable fields are exposed.
 */
typedef struct SsWorldOptions {
    uint32_t num_buffers;              /* sound-buffer (SndBuf) table size         */
    uint32_t max_nodes;
    uint32_t max_graph_defs;           /* synthdef table size                      */
    uint32_t max_wire_bufs;            /* UGen wire-buffer pool                    */
    uint32_t num_audio_bus_channels;
    uint32_t num_input_bus_channels;
    uint32_t num_output_bus_channels;
    uint32_t num_control_bus_channels;
    uint32_t buf_length;               /* control block size, frames              */
    uint32_t real_time_memory_size;    /* RT pool, KB                             */
    uint32_t num_rgens;                /* random generators                       */
    uint32_t load_graph_defs;          /* load synthdefs from fs (0 on self-driven */
                                       /* hosts — no filesystem)                   */
    uint32_t verbosity;
    uint32_t shared_memory_id;         /* native: boost shm id; 0 = none / reuse   */
} SsWorldOptions;

void ss_init(const SsWorldOptions* opts, double sample_rate);

/* ── Layout ────────────────────────────────────────────────────────────────
 * The self-describing arena layout (BufferLayout) and the arena base
 * pointer, for hosts that address the rings directly — the web runtime
 * reads the layout at boot (via the get_buffer_layout export) to locate
 * rings and regions. Hosts using the functions above never need these.
 */
typedef struct SsLanesLayout SsLanesLayout;  /* = BufferLayout, see shared_memory.h */

const SsLanesLayout* ss_lanes_layout(void);
void*                ss_lanes_base(void);

#ifdef __cplusplus
}
#endif
