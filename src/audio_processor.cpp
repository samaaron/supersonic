/*
    SuperSonic - SuperCollider AudioWorklet WebAssembly port
    Copyright (c) 2025 Sam Aaron

    Based on SuperCollider by James McCartney and community
    GPL v3 or later
*/

#include "audio_processor.h"
#include "audio_config.h"
#include "clock_math.h"
#include "osc_debug.h"
#include "SuperClock.h"
#include "EngineClock.h"
#include "OscIngress.h"
#include "IngressCallCtx.h"
#include "ReplyChannel.h"
#include "lanes/lanes_internal.h"   // ss_egress_nrt_write — off-thread debug egress
// Platform macros (SC_COLD_BSS, tiered-memory attributes). Header-only and
// scsynth-free, so it is included in both builds — the no-synth core still
// places the ring arena + scheduler pool in bulk RAM on tiered targets.
#include "SC_Platform.h"

// The ingress the audio-thread drain classifies through (extern in OscIngress.h).
// Defined here — compiled by both native and wasm. Published by the engine at
// init (native: SupersonicEngine::mIngress; wasm: a file-static, see init_memory).
std::atomic<OscIngress*> g_active_ingress{nullptr};

// Published true by a backend that drains the NRT-out ring (the native NRT
// gateway). While true, emit_debug_osc routes OFF-audio-thread debug to the
// locked NRT-out ring instead of the single-writer RT-out ring, keeping RT-out's
// sole-writer invariant. Stays false on single-threaded worklet targets (WASM /
// self-driven device), which have no NRT-out drainer and no second RT-out writer,
// so those always use RT-out. A capability signal, not an __EMSCRIPTEN__ branch.
std::atomic<bool> g_nrt_egress_drained{false};

// The composition-root SuperClock for a worklet self-clock host (WASM + the
// self-driven device). superclock_wasm_init binds the SAB region + the worklet
// TimeSource onto g_active_superclock; superClock() is the host-owned instance.
// (External-clock hosts — native/JUCE — own their SuperClock elsewhere and never
// reach this.) Capability-gated, not __EMSCRIPTEN__, so WASM and the device share
// ONE codepath that every worklet target's tests exercise.
#if SUPERSONIC_WORKLET_CLOCK
extern "C" void superclock_wasm_init(SuperClockState* superclock_state,
                                      const double* ntp_start_time_ptr,
                                      const std::atomic<int32_t>* drift_offset_ptr,
                                      const std::atomic<int32_t>* global_offset_ptr);

static SuperClock& superClock() {
    static SuperClock instance;
    return instance;
}
#endif
#include <emscripten/webaudio.h>
#include <algorithm>
#include <atomic>
#include <cstring>
#include <cstdio>
#include <cstdarg>
#include <cstdlib>
#include <cmath>
#include <limits>

#ifdef __wasm_simd128__
#include <wasm_simd128.h>
#endif

// scsynth includes — only the synth engine (SUPERSONIC_SYNTH) pulls scsynth.
// The no-synth build keeps this file for its IN-ring drain + scheduler fire, so
// every World/scsynth touch below is guarded by #if SUPERSONIC_SYNTH.
#if SUPERSONIC_SYNTH
#include "SC_World.h"              // from synth/include/plugin_interface/
#include "SC_WorldOptions.h"       // from synth/include/server/
#include "SC_Prototypes.h"         // from synth/server/
#include "SC_EngineCore.h"         // from synth/server/ - shared self-driven bring-up
#include "OSC_Packet.h"            // from synth/server/
#include "SC_Reply.h"              // from synth/include/common/
#include "SC_ReplyImpl.hpp"        // from synth/common/ - for ReplyAddress
#include "SC_GraphDef.h"           // from synth/server/
#include "SC_Graph.h"              // from synth/server/
#include "SC_Group.h"              // from synth/server/
#include "SC_HiddenWorld.h"        // from synth/server/
#include "sc_msg_iter.h"           // from synth/include/plugin_interface/
#include "Samp.hpp"                // for sine table initialization
#endif // SUPERSONIC_SYNTH

// Scheduler includes
#include "scheduler/EngineScheduler.h"  // unified scheduler: synth + MIDI/OSC
#include "scheduler/schedule_parse.h"   // shared bundle / "/schedule" parsing
#include "scheduler/fire_due.h"         // shared scheduler fire loop
#include "scheduler/MidiClockOut.h"     // MIDI clock-OUT generation (SuperClock-timed)

// Node tree for SharedArrayBuffer polling
#include "node_tree.h"

// Lanes drain-state reset (init_memory resets ring sequences; the lanes
// consumer state must restart with them) and the shared ring walker the
// IN-ring drain below runs on.
#include "lanes/lanes_internal.h"
#include "lanes/ring_drain.h"

// Pre-allocated heap for RT-safe allocations
#include "supersonic_heap.h"
#include "supersonic_config.h"

// Thread-local RT guard for allocation detection (read by test binary only)
#include "rt_alloc.h"

// Definition of the slot-array pointer declared in shm_audio_buffer.hpp.
// Assigned once at init; AudioOut2 instances read it to locate their slot.
shm_audio_buffer* g_shm_audio_buffers = nullptr;

// Forward declarations (synth perform entry points)
#if SUPERSONIC_SYNTH
int PerformOSCMessage(World* inWorld, int inSize, char* inData, ReplyAddress* inReply);
void PerformOSCBundle(World* inWorld, OSC_Packet* inPacket);
#endif

// Audio-thread /clock handler (the wasm ingress route). Handles the cheap
// clock-core verbs inline and replies via the OUT ring. /clock is the control
// namespace and is always consumed here — it never reaches scsynth. (Native
// registers a different /clock route that forwards to the NRT thread.)
static bool clockCoreRoute(void* routeCtx, const void* callCtx,
                           const uint8_t* data, size_t len) {
    SuperClock* clk = g_active_superclock.load(std::memory_order_acquire);
    if (!clk) return true;   // clock route owns /clock/; drop during startup, don't error
    auto* cc  = static_cast<const DrainCallCtx*>(callCtx);
    auto* chan = static_cast<ReplyChannel*>(routeCtx);   // RT egress, bound at registration
    uint32_t token = cc ? cc->sourceId : 0;              // reply metadata, threaded in
    handleClockCoreOsc(*clk, data, static_cast<uint32_t>(len),
        [chan, token](const uint8_t* d, uint32_t n) {
            if (chan) chan->reply(token, d, n);
        });
    return true;
}

// The scsynth namespace. Kept declared in both builds so the `using namespace
// scsynth;` in the scheduler-bridge functions below resolves either way; only
// the World-typed UnrollOSCPacket forward-declare is synth-only.
namespace scsynth {
#if SUPERSONIC_SYNTH
    // Forward declare UnrollOSCPacket from SC_ComPort.cpp.
    bool UnrollOSCPacket(World* inWorld, int inSize, char* inData, OSC_Packet* inPacket);
#endif
}

// Forward declare ring buffer write function (defined after namespace).
// Dependencies (sequence counter, status-flags word, metrics) are passed in
// rather than read from scsynth globals; default-null args here keep callers
// terse and let the function be unit-tested directly. Defaults live on this
// declaration only — never repeated on the definition.
bool ring_buffer_write(
    uint8_t* buffer_start,
    uint32_t buffer_size,
    std::atomic<int32_t>* head,
    std::atomic<int32_t>* tail,
    std::atomic<int32_t>* sequence,
    uint32_t route,
    uint32_t source_id,
    const void* data,
    uint32_t data_size,
    std::atomic<uint32_t>* status_flags = nullptr,
    PerformanceMetrics* metrics = nullptr
);

// Forward declare table initialization functions
// These must be called manually in standalone WASM builds (no static constructors)
extern "C" void InitializeSynthTables();
extern "C" void InitializeFFTTables();

// Custom errno implementation for single-threaded AudioWorklet
// This bypasses libc's __errno_location which isn't compiled with atomics support
// AudioWorklet is single-threaded so a simple global is sufficient
#ifdef __EMSCRIPTEN__
extern "C" {
    static int global_errno = 0;

    int* __errno_location() {
        return &global_errno;
    }
}
#endif

// Include SuperCollider version info
#include "synth/common/SC_Version.hpp"

// Supersonic version — defined in supersonic_config.h (single source of truth)

// Global pointers
extern "C" {
    // Static ring buffer in WASM data segment, separate from scsynth heap.
    // Size: ~1.4MB (IN: 768KB, OUT: 128KB, DEBUG: 64KB, control/metrics, node tree ~57KB, audio capture ~375KB)
    // 16-byte aligned for shm_audio_buffer slots (alignas(16)) at
    // SHM_AUDIO_START; JS Float64Array access only needs 8.
    // SC_COLD_BSS: on tiered-memory targets (ESP32) this whole arena is bulk
    // RAM, keeping scarce fast RAM for the audio path; no-op on single-tier
    // targets (web/native). See SC_Platform.h.
    alignas(16) SC_COLD_BSS uint8_t ring_buffer_storage[TOTAL_BUFFER_SIZE];

    // Validate at compile time that buffer layout fits in allocated storage
    static_assert(TOTAL_BUFFER_SIZE <= sizeof(ring_buffer_storage),
                  "Buffer layout exceeds allocated storage!");

    // Pre-allocated audio bus staging buffer — sized to the compile-time
    // max so runtime block sizes up to that cap fit without reallocation.
    // (128 * 128 * 4 = 64 KB on web, up to 512 KB on native.) Used as a
    // scratch copy area for channel-major output; we only write
    // g_world->mBufLength samples per channel at runtime.
    alignas(16) float static_audio_bus[sonicpi::kMaxBlockSize * sonicpi::kMaxChannels];

    // IN-ring drain state for the shared walker (ring_drain.h): sequence-gap
    // tracking. Frames are contiguous by wire invariant and parsed in place
    // from the ring — there is no copy buffer. Audio-thread only.
    SsDrainState g_in_drain;

    // Sequence-tracking reset requested off-thread (purge → clear_scheduler
    // runs on a control thread on native); the audio thread applies it before
    // its next drain so g_in_drain stays single-threaded.
    std::atomic<bool> g_in_seq_reset{false};

    // IN-ring flush request (native purge: sleep/wake recovery, cold swap;
    // callable from any thread). Holds the in_sequence snapshot taken at
    // request time, -1 = no request. The drain discards pending frames whose
    // seq predates the snapshot and dispatches everything newer, so the flush
    // removes exactly what was queued at request time. The ring cursors have
    // fixed owners — producers advance head under in_write_lock, the drain
    // owns tail — so the discard runs on the consuming thread via the normal
    // consume path; no cursor is written from the requesting thread.
    std::atomic<int64_t> g_in_flush_below{-1};

    // Audio-thread-only discard state armed from g_in_flush_below: while
    // active, frames with seq before the threshold are consumed undispatched.
    bool     g_in_discard_active = false;
    uint32_t g_in_discard_below  = 0;

    void* g_rt_pool_ptr = nullptr;
    size_t g_rt_pool_size = 0;

    // Optional override for the whole shared-memory arena base. When set
    // (native backend with a public POSIX shm segment), init_memory() points
    // `shared_memory` here instead of the process-local ring_buffer_storage, so
    // the entire shared_memory.h blob — rings, control, metrics, node-tree,
    // audio taps and scope — lives in the segment and is observable
    // cross-process.
    uint8_t* g_external_segment = nullptr;

    uint8_t* shared_memory = nullptr;
    ControlPointers* control = nullptr;
    PerformanceMetrics* metrics = nullptr;
    double* ntp_start_time = nullptr;
    std::atomic<int32_t>* drift_offset = nullptr;
    std::atomic<int32_t>* global_offset = nullptr;
    bool memory_initialized = false;
    World* g_world = nullptr;

    // True only while this thread is inside process_audio(). emit_debug_osc()
    // reads it to keep the RT-out ring single-writer: the audio thread logs to
    // the lock-free RT-out ring; any other thread (watchdog, recovery, boot on
    // native) routes to the locked NRT-out ring, so RT-out never gets a second
    // concurrent writer. Set via AudioThreadScope at the top of process_audio.
    thread_local bool t_on_audio_thread = false;
    struct AudioThreadScope {
        AudioThreadScope()  { t_on_audio_thread = true;  }
        ~AudioThreadScope() { t_on_audio_thread = false; }
    };

    // Unified event scheduler: one timed queue fanning out to the synth graph
    // (run inline) and outbound MIDI/OSC (re-dispatched inline through
    // dispatch()). SC_COLD_BSS: the data pool lives in bulk RAM on tiered
    // targets; the ctor still runs (placing it there).
    SC_COLD_BSS EngineScheduler g_scheduler;

    // File-scope state shared across threads: written by clear_scheduler()
    // on the control thread, read/updated by process_audio() on the audio
    // thread. Relaxed ordering — these are diagnostic counters with no
    // cross-variable invariants.
    std::atomic<uint32_t> local_in_peak{0};
    std::atomic<uint32_t> local_out_peak{0};
    std::atomic<uint32_t> local_nrt_out_peak{0};
    std::atomic<uint32_t> corruption_count{0};
    std::atomic<uint32_t> gap_log_count{0};
    std::atomic<int> late_count{0};

    // Time conversion constants - Based on SC_CoreAudio.cpp
    double g_osc_increment_numerator = 0.0;  // Buffer length in NTP units
    int64_t g_osc_increment = 0;             // NTP units per buffer
    double g_osc_to_samples = 0.0;           // NTP units -> samples conversion
    double g_time_zero_osc = 0.0;            // AudioContext time -> OSC time offset
    bool g_time_initialized = false;         // Have we set up time conversion?

    // Return the base address of the ring buffer
    // JavaScript will use this to calculate all buffer positions
#ifdef __EMSCRIPTEN__
    EMSCRIPTEN_KEEPALIVE
    int get_ring_buffer_base() {
        return reinterpret_cast<int>(ring_buffer_storage);
    }
#endif

    // Real-pointer base of the unified shared-memory arena, valid on every
    // runtime (the int-returning get_ring_buffer_base() above is WASM-only and
    // would truncate a 64-bit native pointer). Returns the arena base set by
    // init_memory(): ring_buffer_storage locally, or the public segment when an
    // external arena was supplied. Used by the shared fixed-inline scope path.
    void* get_shared_memory_base() {
        return shared_memory;
    }

    // Return the buffer layout configuration
    // JavaScript calls this once at initialization to get all buffer constants
    // This ensures JS and C++ stay in sync - single source of truth in C++
    EMSCRIPTEN_KEEPALIVE
    const BufferLayout* get_buffer_layout() {
        return &BUFFER_LAYOUT;
    }

    // Set time offset from JavaScript (AudioContext → NTP conversion)
    // JavaScript calculates this once and passes it to WASM
    EMSCRIPTEN_KEEPALIVE
    void set_time_offset(double offset) {
        g_time_zero_osc = offset;
        g_time_initialized = true;
        ss_log("Time offset set from JavaScript: %.6f", offset);
    }

    // Convert NTP double (seconds since 1900) to OSC timetag (int64)
    inline int64_t ntp_to_osc_timetag(double ntp) {
        return supersonic::ntpToOscTimetag(ntp);
    }

    // The engine's standard reply channel: replies go out the OUT ring. Carries
    // the caller's `origin` token in mReplyData so osc_reply_to_ring_buffer can
    // route the reply back to that client (0 = broadcast). Used for every synth
    // perform (immediate and scheduled), so it lives in one place. ReplyAddress
    // is a scsynth type, so this is synth-only.
#if SUPERSONIC_SYNTH
    ReplyAddress ring_reply(uint32_t origin) {
        ReplyAddress r = {};
        r.mProtocol  = kWeb;
        r.mReplyFunc = osc_reply_to_ring_buffer;
        r.mReplyData = reinterpret_cast<void*>(static_cast<uintptr_t>(origin));
        return r;
    }
#endif

#if SUPERSONIC_WORKLET_CLOCK && SUPERSONIC_SYNTH
    // The RT egress as a generic ReplyChannel: emit one OSC to the OUT ring,
    // routed by the per-call token. Lets the worklet audio-thread control routes
    // (the /clock namespace) reply through the backend contract without any scsynth
    // reply type. Shared by every worklet synth host (WASM + the self-driven device).
    void rt_reply_emit(void*, uint32_t token, const uint8_t* osc, uint32_t len) {
        ReplyAddress addr = ring_reply(token);
        osc_reply_to_ring_buffer(
            &addr, const_cast<char*>(reinterpret_cast<const char*>(osc)),
            static_cast<int>(len));
    }
    ReplyChannel g_rt_reply{ &rt_reply_emit, nullptr };
#endif

    // Helper: Update scheduler depth metric and peak tracking
    static inline void update_scheduler_depth_metric(uint32_t depth) {
        if (!metrics) {
            return;
        }

        metrics->scheduler_queue_depth.store(depth, std::memory_order_relaxed);

        uint32_t observed = metrics->scheduler_queue_max.load(std::memory_order_relaxed);
        while (depth > observed &&
               !metrics->scheduler_queue_max.compare_exchange_weak(
                   observed, depth, std::memory_order_relaxed, std::memory_order_relaxed)) {
            // observed updated with current value by compare_exchange_weak
        }
    }

    static inline void increment_scheduler_drop_metric() {
        if (!metrics) {
            return;
        }
        metrics->scheduler_queue_dropped.fetch_add(1, std::memory_order_relaxed);
    }

    // Clear the event scheduler (all pending synth bundles + outbound MIDI/OSC).
    // Called from the worklet JS layer (via postMessage flag) to flush
    // everything pending without going through the ring buffer.
    //
    // Note: the IN ring buffer is drained separately by the JS worklet in its
    // message handler (immediately on receiving clearSched), not here. Draining
    // eagerly in JS ensures stale messages are discarded before the ack is sent,
    // so new messages written after purge() resolves are not affected.
    // Request that the audio thread discard everything pending in the IN ring
    // as of NOW: frames sequenced before this snapshot are dropped by the
    // drain, later ones dispatch normally. Callable from any thread; the
    // caller only requests — the consuming thread applies (see g_in_flush_below).
    void ss_ingress_flush_request() {
        if (!memory_initialized || !control) return;
        g_in_flush_below.store(
            control->in_sequence.load(std::memory_order_acquire),
            std::memory_order_release);
    }

    EMSCRIPTEN_KEEPALIVE
    void clear_scheduler() {
        g_in_seq_reset.store(true, std::memory_order_relaxed);
        local_in_peak.store(0, std::memory_order_relaxed);
        local_out_peak.store(0, std::memory_order_relaxed);
        local_nrt_out_peak.store(0, std::memory_order_relaxed);
        corruption_count.store(0, std::memory_order_relaxed);
        gap_log_count.store(0, std::memory_order_relaxed);
        late_count.store(0, std::memory_order_relaxed);
        update_scheduler_depth_metric(0);
        g_scheduler.requestClear();
    }

    // ── The two engine entry points, shared by every host (WASM/native/embedded).
    // Once OSC bytes leave the IN ring, everything funnels through these. ─────────

    // Route an OSC message to its handler NOW (synth inline / control / backend).
    // `when` is the message's intended OSC timetag (0 = immediate); the synth
    // backend derives its sub-block sample offset from it — every other handler
    // ignores it. `token` is the sender/origin, threaded through for reply routing.
    // `blockTime` is this block's start in OSC time — the reference the synth
    // backend subtracts `when` from for its sub-block offset. Threaded explicitly
    // (not a global) so the offset's only input is the call ctx; 0 for immediate.
    void dispatch(const uint8_t* osc, uint32_t len, uint32_t token,
                  int64_t when, int64_t blockTime) {
        const DrainCallCtx cc{ token, when, blockTime };
        OscIngress* ig = g_active_ingress.load(std::memory_order_acquire);
        if (ig && ig->ingest(osc, len, &cc)) return;
        // Nothing claimed it: no matching route and no default registered (e.g. a
        // synth message in a no-synth build). Drop, and log rate-limited so junk
        // can't flood the audio-thread log.
        static std::atomic<uint32_t> noBackendLog{0};
        if (noBackendLog.fetch_add(1, std::memory_order_relaxed) < 16) {
            uint32_t a = 0; while (a < len && osc[a] != '\0') ++a;
            ss_log("ERROR: no backend for OSC %.*s — dropped",
                   static_cast<int>(a), reinterpret_cast<const char*>(osc));
        }
    }

    // Defer an OSC message until `when` (its OSC timetag), carrying its sender
    // token; the fire loop drains due events and calls dispatch(osc, token, when).
    // `tag` groups events for /sched/flush.
    //
    // Fail-open: the IN-ring drain is in-order, so back-pressuring here (leaving
    // the frame in the ring) would head-of-line-block every later message —
    // including immediate commands and the /sched/flush that frees space. A full
    // scheduler holds far-future events that free no space for many blocks, so
    // that stall is unbounded. An un-schedulable event is therefore dropped (and
    // counted), never retained, so command intake never wedges. The scheduler is
    // sized so that, within a sane lookahead, this drop does not happen in normal
    // use; a drop means the producer scheduled further ahead than the pool holds.
    void scheduled_dispatch(const uint8_t* osc, uint32_t len, uint32_t token,
                            int64_t when, uint32_t tag) {
        if (len > EngineScheduler::kMaxPayload) {
            ss_log("WARNING: scheduled message too large (%u bytes, max %u) - dropped",
                   len, EngineScheduler::kMaxPayload);
            increment_scheduler_drop_metric();
            return;
        }
        if (g_scheduler.full() || !g_scheduler.addScheduled(when, tag, token, osc, len)) {
            ss_log("WARNING: scheduler full (%d events) - scheduled message dropped",
                   g_scheduler.size());
            increment_scheduler_drop_metric();
            return;
        }
        update_scheduler_depth_metric(g_scheduler.size());
    }

    // Initialize memory pointers. The arena is the public POSIX segment when
    // the native backend supplied one (g_external_segment), else the in-band
    // ring_buffer_storage (WASM, and headless native with no shm). Either way
    // every region is addressed by its shared_memory.h offset from this base.
    EMSCRIPTEN_KEEPALIVE
    void init_memory(double sample_rate) {
        shared_memory = g_external_segment ? g_external_segment : ring_buffer_storage;
        control = reinterpret_cast<ControlPointers*>(shared_memory + CONTROL_START);
        // Metrics live in the arena at their fixed offset — which is the public
        // segment when one was supplied, so external observers read them with
        // no redirect.
        metrics = reinterpret_cast<PerformanceMetrics*>(shared_memory + METRICS_START);

        // Timing pointers
        ntp_start_time = reinterpret_cast<double*>(shared_memory + NTP_START_TIME_START);
        drift_offset = reinterpret_cast<std::atomic<int32_t>*>(shared_memory + DRIFT_OFFSET_START);
        global_offset = reinterpret_cast<std::atomic<int32_t>*>(shared_memory + GLOBAL_OFFSET_START);

        // Initialize timing (NTP_START_TIME is write-once from JavaScript, don't touch it)
        // *ntp_start_time is written by JavaScript after AudioContext starts
        drift_offset->store(0, std::memory_order_relaxed);
        global_offset->store(0, std::memory_order_relaxed);

#if SUPERSONIC_WORKLET_CLOCK
        // Hand SAB pointers to the composition-root SuperClock at boot — shared by
        // every worklet self-clock host (WASM + the self-driven device). Publish the
        // instance first: superclock_wasm_init binds the SAB region + the worklet
        // clock onto whatever g_active_superclock points at. (External-clock hosts —
        // native/JUCE — bind their own SuperClock elsewhere, never here.)
        SuperClockState* superclock_state =
            reinterpret_cast<SuperClockState*>(shared_memory + SUPERCLOCK_STATE_START);
        SuperClockState::initDefaults(*superclock_state);

        g_active_superclock.store(&superClock(), std::memory_order_release);
        superclock_wasm_init(superclock_state, ntp_start_time, drift_offset, global_offset);
#endif

#if SUPERSONIC_WORKLET_CLOCK && SUPERSONIC_SYNTH
        // The worklet audio-thread ingress: /clock is the only control namespace
        // here (no NRT thread for /supersonic or Link-session verbs). The synth
        // plane is the default route, so unmatched packets perform inline. ONE
        // codepath for every worklet synth host (WASM + the self-driven device).
        static OscIngress worklet_ingress;
        if (worklet_ingress.routeCount() == 0) {
            worklet_ingress.registerRoute("/clock/", &clockCoreRoute, &g_rt_reply);
            worklet_ingress.setDefault(&ss_synth_default_route, nullptr);
        }
        g_active_ingress.store(&worklet_ingress, std::memory_order_release);
#endif

        // Initialize all atomics to 0
        control->in_head.store(0, std::memory_order_relaxed);
        control->in_tail.store(0, std::memory_order_relaxed);
        control->out_head.store(0, std::memory_order_relaxed);
        control->out_tail.store(0, std::memory_order_relaxed);
        control->nrt_out_head.store(0, std::memory_order_relaxed);
        control->nrt_out_tail.store(0, std::memory_order_relaxed);
        control->in_sequence.store(0, std::memory_order_relaxed);
        control->out_sequence.store(0, std::memory_order_relaxed);
        control->nrt_out_sequence.store(0, std::memory_order_relaxed);
        control->status_flags.store(STATUS_OK, std::memory_order_relaxed);
        control->in_write_lock.store(0, std::memory_order_relaxed);  // 0 = unlocked

        // Ring sequences restarted → restart the lanes drains' gap tracking,
        // and the IN drain's alongside. A flush requested against the old
        // ring must not carry over and discard fresh writes.
        ss_lanes_reset_drains();
        g_in_drain.lastSeq = -1;
        g_in_flush_below.store(-1, std::memory_order_relaxed);
        g_in_discard_active = false;

        // Initialize metrics
        metrics->process_count.store(0, std::memory_order_relaxed);
        metrics->messages_processed.store(0, std::memory_order_relaxed);
        metrics->messages_dropped.store(0, std::memory_order_relaxed);
        metrics->scheduler_queue_depth.store(0, std::memory_order_relaxed);
        metrics->scheduler_queue_max.store(0, std::memory_order_relaxed);
        metrics->scheduler_queue_dropped.store(0, std::memory_order_relaxed);
        metrics->messages_sequence_gaps.store(0, std::memory_order_relaxed);
        metrics->scheduler_lates.store(0, std::memory_order_relaxed);

        // Initialize late timing diagnostics
        metrics->scheduler_max_late_ms.store(0, std::memory_order_relaxed);
        metrics->scheduler_last_late_ms.store(0, std::memory_order_relaxed);
        metrics->scheduler_last_late_tick.store(0, std::memory_order_relaxed);

        // Initialize node tree memory
        // All entries start with id = -1 (empty slot)
        // Using memset with 0xFF sets all bytes to 0xFF, which is -1 for signed int32
        uint8_t* node_tree_ptr = shared_memory + NODE_TREE_START;
        memset(node_tree_ptr, 0xFF, NODE_TREE_SIZE);

        // Initialize header properly (count=0, version=0, dropped_count=0)
        NodeTreeHeader* tree_header = reinterpret_cast<NodeTreeHeader*>(node_tree_ptr);
        tree_header->node_count.store(0, std::memory_order_relaxed);
        tree_header->version.store(0, std::memory_order_relaxed);
        tree_header->dropped_count.store(0, std::memory_order_relaxed);

        // Initialize free list and hash table for O(1) node tree operations.
        // The index machinery (node_tree.cpp) only exists in the synth build;
        // the empty header written above keeps the SAB layout valid either way.
#if SUPERSONIC_SYNTH
        NodeTree_InitIndices();
#endif

        ss_log("[NodeTree] Initialized at offset %u, size %u bytes",
                     NODE_TREE_START, NODE_TREE_SIZE);

        // Audio buffer slot array. Slot 0 carries the master output mix
        // and is written by the post-block hook below when `enabled` is
        // set; slots 1..N-1 are written by AudioOut2 UGens (each calls
        // activate() from its Ctor). The slots live in the arena at their
        // fixed offset — the public segment when present, so the Sonic Pi
        // recorder reads them with no redirect.
        shm_audio_buffer* slots = reinterpret_cast<shm_audio_buffer*>(
            shared_memory + SHM_AUDIO_START);
        memset(static_cast<void*>(slots), 0,
               MAX_SHM_AUDIO_BUFFERS * sizeof(shm_audio_buffer));
        slots[SHM_AUDIO_MASTER_SLOT].sample_rate = static_cast<uint32_t>(sample_rate);
        slots[SHM_AUDIO_MASTER_SLOT].channels = SHM_AUDIO_CHANNELS;
        slots[SHM_AUDIO_MASTER_SLOT].capacity_frames = SHM_AUDIO_FRAMES;
        g_shm_audio_buffers = slots;

        // Scope global header (fixed-inline scope). The per-slot writer
        // (SC_World.cpp) sets up its own slot on demand; here we just publish
        // the layout so cross-process observers know the geometry. Slots
        // themselves start zeroed (state=free) from the segment memset.
        uint8_t* scope_hdr = shared_memory + SHM_SCOPE_START;
        reinterpret_cast<std::atomic<uint32_t>*>(scope_hdr + 0)->store(
            SHM_SCOPE_MAX_SCOPES, std::memory_order_relaxed);     // maxScopes
        reinterpret_cast<std::atomic<uint32_t>*>(scope_hdr + 4)->store(
            0, std::memory_order_relaxed);                        // activeCount
        reinterpret_cast<std::atomic<uint32_t>*>(scope_hdr + 8)->store(
            SHM_SCOPE_FRAMES_PER_SCOPE, std::memory_order_relaxed); // framesPerScope
        reinterpret_cast<std::atomic<uint32_t>*>(scope_hdr + 12)->store(
            0, std::memory_order_relaxed);                        // version

        // Initialize scope buffers
        {
            uint8_t* scopeBase = shared_memory + SHM_SCOPE_START;
            memset(scopeBase, 0, SHM_SCOPE_TOTAL_SIZE);
            auto* maxScopes = reinterpret_cast<uint32_t*>(scopeBase + 0);
            auto* framesPerScope = reinterpret_cast<uint32_t*>(scopeBase + 8);
            *maxScopes = SHM_SCOPE_MAX_SCOPES;
            *framesPerScope = SHM_SCOPE_FRAMES_PER_SCOPE;
        }

        // Enable ss_log
        memory_initialized = true;

        // Boot message shown after ASCII art below

#if SUPERSONIC_SYNTH
        // Read worldOptions from the arena at WORLD_OPTIONS_START. Must use
        // `shared_memory` (the arena — the public segment when one was supplied,
        // else ring_buffer_storage), NOT ring_buffer_storage directly: the host
        // (initialiseWorld) writes the options into the same arena base, so
        // reading from ring_buffer_storage would see zeros when a segment is in
        // use. This offset is outside the ring buffers, so OSC can't overwrite it.
        uint32_t* worldOptionsPtr = (uint32_t*)(shared_memory + WORLD_OPTIONS_START);

        // Configure World for NRT mode (externally driven by AudioWorklet)
        // Values come from JS (scsynth_options.js) via SharedArrayBuffer
        WorldOptions options;
        options.mRealTime = false;                    // NRT mode - externally driven, no audio driver
        options.mMemoryLocking = false;               // No memory locking in WASM
        options.mNumBuffers = worldOptionsPtr[0];                   // From JS
        options.mMaxNodes = worldOptionsPtr[1];                     // From JS
        options.mMaxGraphDefs = worldOptionsPtr[2];                 // From JS
        options.mMaxWireBufs = worldOptionsPtr[3];                  // From JS
        options.mNumAudioBusChannels = worldOptionsPtr[4];          // From JS
        options.mNumInputBusChannels = worldOptionsPtr[5];          // From JS
        options.mNumOutputBusChannels = worldOptionsPtr[6];         // From JS
        options.mNumControlBusChannels = worldOptionsPtr[7];        // From JS
        options.mBufLength = worldOptionsPtr[8];                    // From JS (128 for WebAudio)
        options.mRealTimeMemorySize = worldOptionsPtr[9];           // From JS
        options.mNumRGens = worldOptionsPtr[10];                    // From JS
        // worldOptionsPtr[11] = realTime (ignored, always false for WASM)
        // worldOptionsPtr[12] = memoryLocking (ignored, always false for WASM)
        options.mLoadGraphDefs = worldOptionsPtr[13];               // From JS
        options.mPreferredSampleRate = worldOptionsPtr[14] > 0
            ? worldOptionsPtr[14]
            : (uint32_t)sample_rate;                                // From JS or AudioContext
        // worldOptionsPtr[15] = verbosity
        options.mVerbosity = worldOptionsPtr[15];                   // From JS
#ifdef __EMSCRIPTEN__
        {
            uint32_t rtPoolOffset = worldOptionsPtr[sonicpi::WorldOpts::kWebRtPoolOffset];
            uint32_t rtPoolBytes = options.mRealTimeMemorySize * 1024;
            if (rtPoolOffset > 0 && rtPoolBytes > 0) {
                g_rt_pool_ptr = (void*)(shared_memory + rtPoolOffset);
                g_rt_pool_size = rtPoolBytes;
                memset(g_rt_pool_ptr, 0, g_rt_pool_size);
                if (options.mVerbosity > 0)
                    ss_log("RT_POOL: pre-allocated at offset %u (%uMB) size %uMB",
                        rtPoolOffset, rtPoolOffset / (1024*1024), rtPoolBytes / (1024*1024));
            }
        }
#endif
#ifndef __EMSCRIPTEN__
        // Native: sharedMemoryID is written at kNativeSharedMemoryID (index 17)
        // by JuceAudioCallback; the named constant locks this to the write site.
        options.mSharedMemoryID = worldOptionsPtr[sonicpi::WorldOpts::kNativeSharedMemoryID];  // UDP port for boost shm (native only)
        extern void* g_external_shared_memory;
        if (g_external_shared_memory) {
            options.mExternalSharedMemory = g_external_shared_memory;
            options.mSharedMemoryID = 0;  // don't create new — reuse external
        }
#endif

#ifdef __EMSCRIPTEN__
        // REGION INTEGRITY CHECK: verify malloc heap doesn't extend into the RT pool.
        // The WASM linear memory layout is:
        //   [heap 0..sbrk] ... [RT pool at fixed offset] [buffer pool after RT pool]
        // If malloc grows the heap past the RT pool start, allocations overlap.
        {
            extern void* sbrk(intptr_t);
            uintptr_t heap_end = (uintptr_t)sbrk(0);
            uintptr_t rt_start = g_rt_pool_ptr ? (uintptr_t)g_rt_pool_ptr : 0;
            uintptr_t rt_end = rt_start + g_rt_pool_size;

            if (rt_start > 0 && heap_end > rt_start) {
                ss_log("FATAL: WASM heap (sbrk=0x%x) overlaps RT pool (start=0x%x) — reduce heap usage or increase rtPoolOffset",
                    (uint32_t)heap_end, (uint32_t)rt_start);
                control->status_flags.fetch_or(STATUS_WASM_ERROR, std::memory_order_relaxed);
                return;
            }
            if (options.mVerbosity > 0)
                ss_log("MEMORY OK: heap<0x%x rt=[0x%x,0x%x) buf=0x%x+",
                    (uint32_t)heap_end, (uint32_t)rt_start, (uint32_t)rt_end, (uint32_t)rt_end);
        }
#endif

        // Initialize pre-allocated heap (no-op on WASM, creates AllocPool on
        // native). The argument sizes the Fast (internal-SRAM) initial area; on
        // desktop SUPERSONIC_HEAP_FAST_SIZE == SUPERSONIC_HEAP_SIZE, so the pool
        // is one region as before (see memory_profile.h).
        supersonic_heap_init(SUPERSONIC_HEAP_FAST_SIZE);

        // Create + start the World via the shared engine-core bring-up: World_New,
        // sample rate, wire buffers, and the bus/wire-buffer sanity checks (see
        // SC_EngineCore.h). options.mPreferredSampleRate was set above.
        try {
            const char* err = nullptr;
            g_world = EngineCore_New(&options, &err);
            if (!g_world) {
                ss_log("ERROR: engine bring-up failed: %s", err ? err : "unknown");
                control->status_flags.fetch_or(STATUS_WASM_ERROR, std::memory_order_relaxed);
                return;
            }
        } catch (const std::exception& e) {
            ss_log("ERROR: engine bring-up threw exception: %s", e.what());
            control->status_flags.fetch_or(STATUS_WASM_ERROR, std::memory_order_relaxed);
            return;
        } catch (...) {
            ss_log("ERROR: engine bring-up threw unknown exception");
            control->status_flags.fetch_or(STATUS_WASM_ERROR, std::memory_order_relaxed);
            return;
        }

        // Re-assert the running sample rate from the AudioContext rate. EngineCore
        // sets it from options.mPreferredSampleRate (normally identical); this keeps
        // the AudioContext rate authoritative regardless of the options value.
        World_SetSampleRate(g_world, sample_rate);

        // Zero the static audio bus
        memset(static_audio_bus, 0, sizeof(static_audio_bus));

        // Initialize scheduler time constants (from SC_CoreAudio.cpp:426-428)
        int buf_length = g_world->mBufLength;  // 128 samples
        g_osc_increment_numerator = (double)buf_length;
        g_osc_increment = (int64_t)(g_osc_increment_numerator / sample_rate * 4294967296.0);
        g_osc_to_samples = sample_rate / 4294967296.0;

        // Publish cross-platform system info into the metrics struct (slots
        // 58-64). Constant for the session; written once now that the World
        // exists. Runs on every runtime — native's initialiseWorld() also
        // routes through init_memory().
        metrics->supersonic_version_major.store(SUPERSONIC_VERSION_MAJOR, std::memory_order_relaxed);
        metrics->supersonic_version_minor.store(SUPERSONIC_VERSION_MINOR, std::memory_order_relaxed);
        metrics->supersonic_version_patch.store(SUPERSONIC_VERSION_PATCH, std::memory_order_relaxed);
        metrics->audio_sample_rate.store(static_cast<uint32_t>(sample_rate + 0.5), std::memory_order_relaxed);
        metrics->audio_block_size.store(static_cast<uint32_t>(buf_length), std::memory_order_relaxed);
        metrics->audio_output_channels.store(options.mNumOutputBusChannels, std::memory_order_relaxed);
        metrics->audio_input_channels.store(options.mNumInputBusChannels, std::memory_order_relaxed);

        // Clear scheduler
        g_scheduler.clear();
        update_scheduler_depth_metric(0);

        // Add root group to node tree (it was created during World_New but doesn't trigger Node_StateMsg)
        if (g_world->mTopGroup) {
            uint8_t* node_tree_ptr = shared_memory + NODE_TREE_START;
            NodeTreeHeader* tree_header = reinterpret_cast<NodeTreeHeader*>(node_tree_ptr);
            NodeEntry* tree_entries = reinterpret_cast<NodeEntry*>(node_tree_ptr + NODE_TREE_HEADER_SIZE);
            NodeTree_Add(&g_world->mTopGroup->mNode, tree_header, tree_entries);
        }


#ifdef __EMSCRIPTEN__
        ss_log(R"(
░█▀▀░█░█░█▀█░█▀▀░█▀▄░█▀▀░█▀█░█▀█░▀█▀░█▀▀
░▀▀█░█░█░█▀▀░█▀▀░█▀▄░▀▀█░█░█░█░█░░█░░█░░
░▀▀▀░▀▀▀░▀░░░▀▀▀░▀░▀░▀▀▀░▀▀▀░▀░▀░▀▀▀░▀▀▀)");
        ss_log("v%d.%d.%d (scsynth %d.%d.%d)",
                     SUPERSONIC_VERSION_MAJOR, SUPERSONIC_VERSION_MINOR, SUPERSONIC_VERSION_PATCH,
                     SC_VersionMajor, SC_VersionMinor, SC_VersionPatch);
        {
            const char* transport_mode =
                worldOptionsPtr[sonicpi::WorldOpts::kWebTransportFlag] ? "PM" : "SAB";
            ss_log("%.0fkHz %dch [%s]",
                         sample_rate / 1000, options.mNumOutputBusChannels, transport_mode);
        }
        ss_log("");
        ss_log("> scsynth ready...");
#endif
#else  // !SUPERSONIC_SYNTH
        // No-synth core: no World, no audio render. The scheduler/router still
        // runs — derive the block-time constants from the AudioContext rate and a
        // default block size so the fire loop's OSC-time window is correct.
        const int buf_length = sonicpi::kDefaultBlockSize;
        g_osc_increment_numerator = (double)buf_length;
        g_osc_increment = (int64_t)(g_osc_increment_numerator / sample_rate * 4294967296.0);
        g_osc_to_samples = sample_rate / 4294967296.0;

        metrics->supersonic_version_major.store(SUPERSONIC_VERSION_MAJOR, std::memory_order_relaxed);
        metrics->supersonic_version_minor.store(SUPERSONIC_VERSION_MINOR, std::memory_order_relaxed);
        metrics->supersonic_version_patch.store(SUPERSONIC_VERSION_PATCH, std::memory_order_relaxed);
        metrics->audio_sample_rate.store(static_cast<uint32_t>(sample_rate + 0.5), std::memory_order_relaxed);
        metrics->audio_block_size.store(static_cast<uint32_t>(buf_length), std::memory_order_relaxed);

        g_scheduler.clear();
        update_scheduler_depth_metric(0);
#endif // SUPERSONIC_SYNTH
    }

#if !defined(__EMSCRIPTEN__) && SUPERSONIC_SYNTH
    // destroy_world / rebuild_world — for native cold swap (device sample rate change).
    // Tears down the World (keeping UGen plugins loaded) and rebuilds with new sample rate.
    // Synth-only — the no-synth core has no World to cold-swap. The sole caller
    // (SupersonicEngine) is itself excluded from the no-synth build.
    // Cold-swap entry/exit — the engine's state-transition lines
    // ([supersonic] state: restarting -> running) already record when these
    // fire, so separate tracing here is redundant.
    // Notify-client registrations (hw->mUsers) gate /n_go//n_end delivery and
    // live in the World, so a destroy/rebuild empties them — clients stop
    // receiving node notifications until they re-/notify. Capture the registered
    // (token, clientID) pairs across the rebuild and re-insert them, so notify
    // survives transparently. The transport's token→address map persists, so the
    // saved tokens stay routable.
    std::vector<std::pair<uint32_t, int>> g_savedNotifyClients;

    void capture_notify_clients() {
        g_savedNotifyClients.clear();
        if (!g_world || !g_world->hw || !g_world->hw->mUsers) return;
        HiddenWorld* hw = g_world->hw;
        for (auto addr : *hw->mUsers) {
            const uint32_t token =
                static_cast<uint32_t>(reinterpret_cast<uintptr_t>(addr.mReplyData));
            int clientID = 0;
            auto it = hw->mClientIDdict->find(addr);
            if (it != hw->mClientIDdict->end()) clientID = it->second;
            g_savedNotifyClients.emplace_back(token, clientID);
        }
    }

    void restore_notify_clients() {
        if (!g_world || !g_world->hw || !g_world->hw->mUsers) { g_savedNotifyClients.clear(); return; }
        HiddenWorld* hw = g_world->hw;
        for (const auto& c : g_savedNotifyClients) {
            ReplyAddress r = ring_reply(c.first);
            bool present = false;
            for (auto a : *hw->mUsers) { if (a == r) { present = true; break; } }
            if (present) continue;
            // Mirror NotifyCmd::Stage2's registration, preserving the original
            // clientID so the host's view (node-id range, etc.) stays consistent.
            hw->mClientIDdict->insert(std::make_pair(r, c.second));
            hw->mUsers->insert(r);
            auto& avail = *hw->mAvailableClientIDs;
            avail.erase(std::remove(avail.begin(), avail.end(), c.second), avail.end());
        }
        g_savedNotifyClients.clear();
    }

    void destroy_world() {
        if (g_world) {
            capture_notify_clients();        // preserve /notify across the rebuild
            World_Cleanup(g_world, false);  // false = keep UGen plugins loaded
            g_world = nullptr;
        }
        supersonic_heap_destroy();
        g_scheduler.clear();
        update_scheduler_depth_metric(0);
        g_in_seq_reset.store(true, std::memory_order_relaxed);
    }

    void rebuild_world(double sample_rate) {
        // Re-read worldOptions from ring_buffer_storage + WORLD_OPTIONS_START
        // (caller must update opts[WorldOpts::kSampleRate] before calling)
        init_memory(sample_rate);
        restore_notify_clients();            // re-register the pre-rebuild clients
    }

    // Mirror of init_memory for engine shutdown: tear down the World and drop
    // the engine's global view of the arena while it is still mapped, so the
    // lanes guards (memory_initialized / control) reject post-shutdown calls
    // instead of touching a freed or unmapped segment. Saved /notify clients
    // only make sense across a cold-swap rebuild, never across engines.
    void teardown_memory() {
        destroy_world();
        g_savedNotifyClients.clear();
        memory_initialized = false;
        shared_memory = nullptr;
        control = nullptr;
        metrics = nullptr;
    }
#endif

    // Main audio processing function - called once per block (the lanes tick,
    // ss_tick, wraps this).
    // current_time: AudioContext.currentTime (WASM) or wall-clock NTP (native)
    // active_output_channels / active_input_channels: live channel counts
    EMSCRIPTEN_KEEPALIVE
    bool process_audio(double current_time, uint32_t active_output_channels, uint32_t active_input_channels) {
        AudioThreadScope _audio_thread_scope;   // this thread owns RT-out for ss_log routing
#if SUPERSONIC_SYNTH
        if (!memory_initialized || !g_world) {
            return true; // Not ready or world destroyed during cold swap — output silence
        }
#else
        // No-synth core: there is no World; the scheduler/router still ticks.
        if (!memory_initialized) {
            return true;
        }
#endif

        if (!metrics) {
            return false;
        }

        g_scheduler.drainPendingClear();

        // Calculate current NTP time from components
        // currentNTP = audioContextTime + ntp_start + (drift_us/1000000) + (global_ms/1000)
        // Read ntp_start_time directly from shared memory every frame
        // (no caching - ensures immediate response to timing resync after resume)
        // WASM derives NTP via SuperClock; on native, current_time is
        // already the SuperClock-derived NTP from JuceAudioCallback.
#if SUPERSONIC_WORKLET_CLOCK
        // Worklet self-clock host (WASM + the self-driven device): derive NTP from
        // the sample clock via the bound SuperClock, and mirror the readout into the
        // cross-platform clock metrics (slots 65-68). External-clock hosts (native/
        // JUCE) take the #else: current_time is already the SuperClock-derived NTP
        // from the callback.
        const double current_ntp = superClock().nowAt(current_time);
#else
        const double current_ntp = current_time;
#endif

        uint32_t pc = metrics->process_count.fetch_add(1, std::memory_order_relaxed) + 1;

        // Publish native-only engine stats (synthdef count, allocated buffers)
        // at a low rate — the synthdef count is O(1) but the SndBuf scan is
        // O(numBufs), so throttle to ~every 64 blocks to keep the audio thread
        // light. Declared in SC_World.cpp.
#if SUPERSONIC_SYNTH
        extern void World_UpdateNativeStats(World*);
        if (g_world && (pc & 63u) == 0u) World_UpdateNativeStats(g_world);
#endif

        // Host telemetry. Sample the ring fill every block so peaks stay a true
        // high-water mark (a burst can fill and drain within a few blocks); this is cheap
        // — the control reads overlap the drain and peaks are local. Flush the SAB (clock
        // readout + ring used/peaks) only at the poll rate below: a consumer reads it at
        // display rate, and per-block flushing is costly where the metrics struct is in
        // slow memory (PSRAM on ESP32). process_count stays per-block as the block counter.
        {
            int32_t in_head = control->in_head.load(std::memory_order_relaxed);
            int32_t in_tail = control->in_tail.load(std::memory_order_relaxed);
            uint32_t in_used = (in_head - in_tail + IN_BUFFER_SIZE) % IN_BUFFER_SIZE;
            if (in_used > local_in_peak.load(std::memory_order_relaxed))
                local_in_peak.store(in_used, std::memory_order_relaxed);

            int32_t out_head = control->out_head.load(std::memory_order_relaxed);
            int32_t out_tail = control->out_tail.load(std::memory_order_relaxed);
            uint32_t out_used = (out_head - out_tail + OUT_BUFFER_SIZE) % OUT_BUFFER_SIZE;
            if (out_used > local_out_peak.load(std::memory_order_relaxed))
                local_out_peak.store(out_used, std::memory_order_relaxed);

            int32_t nrt_out_head = control->nrt_out_head.load(std::memory_order_relaxed);
            int32_t nrt_out_tail = control->nrt_out_tail.load(std::memory_order_relaxed);
            uint32_t nrt_out_used = (nrt_out_head - nrt_out_tail + NRT_OUT_BUFFER_SIZE) % NRT_OUT_BUFFER_SIZE;
            if (nrt_out_used > local_nrt_out_peak.load(std::memory_order_relaxed))
                local_nrt_out_peak.store(nrt_out_used, std::memory_order_relaxed);

            // Flush period in blocks, derived once from the audio rate to target ~30Hz
            // (tracks sample rate / block size instead of a fixed divisor); 16 until the
            // audio config is published.
            static uint32_t s_flush_period = 0;
            if (s_flush_period == 0u) {
                const uint32_t blk = metrics->audio_block_size.load(std::memory_order_relaxed);
                const uint32_t sr  = metrics->audio_sample_rate.load(std::memory_order_relaxed);
                if (blk && sr) {            // round(sr / (30 * blk)) in integer math
                    uint32_t p = (sr + 15u * blk) / (30u * blk);
                    s_flush_period = (p < 1u) ? 1u : p;
                }
            }
            const uint32_t flush_period = (s_flush_period != 0u) ? s_flush_period : 16u;
            if ((pc % flush_period) == 0u) {
#if SUPERSONIC_WORKLET_CLOCK
                superClock().publishClockMetrics(metrics, current_ntp, 4.0);
#endif
                metrics->in_buffer_used_bytes.store(in_used, std::memory_order_relaxed);
                metrics->in_buffer_peak_bytes.store(local_in_peak.load(std::memory_order_relaxed), std::memory_order_relaxed);
                metrics->out_buffer_used_bytes.store(out_used, std::memory_order_relaxed);
                metrics->out_buffer_peak_bytes.store(local_out_peak.load(std::memory_order_relaxed), std::memory_order_relaxed);
                metrics->nrt_out_buffer_used_bytes.store(nrt_out_used, std::memory_order_relaxed);
                metrics->nrt_out_buffer_peak_bytes.store(local_nrt_out_peak.load(std::memory_order_relaxed), std::memory_order_relaxed);
            }
        }

        // Process incoming OSC messages. The walk — header validation,
        // untrusted-cursor repair, padding markers, gap tracking, tail resync on
        // corruption — is the shared lanes walker (ring_drain.h); only the
        // perform/schedule/classify policy lives here, in the callback. The drain
        // and the scheduler fire below run on every build; only the audio render
        // (gated by #if SUPERSONIC_SYNTH) needs a World. ON keeps the original
        // `if (g_world)` guard so post-init behaviour is unchanged.
#if SUPERSONIC_SYNTH
        if (g_world)
#endif
        {
            // Flush request (purge): arm the discard threshold here so
            // g_in_discard_* stays audio-thread-only, like g_in_drain below.
            {
                const int64_t below =
                    g_in_flush_below.exchange(-1, std::memory_order_acquire);
                if (below >= 0) {
                    g_in_discard_active = true;
                    g_in_discard_below  = static_cast<uint32_t>(below);
                }
            }

            // Off-thread reset request (purge → clear_scheduler): apply it
            // here so g_in_drain stays audio-thread-only.
            if (g_in_seq_reset.exchange(false, std::memory_order_relaxed))
                g_in_drain.lastSeq = -1;

            // Bound per block to stay within the audio budget.
            constexpr uint32_t MAX_MESSAGES_PER_FRAME = 32;

            // Snapshot the gap counter so losses this block can be surfaced
            // in the debug channel (the walker only counts them).
            uint32_t gaps_before =
                metrics->messages_sequence_gaps.load(std::memory_order_relaxed);

            SsDrainStop stop = SsDrainStop::Empty;
            ss_drain_ring(
                shared_memory + IN_BUFFER_START, IN_BUFFER_SIZE,
                &control->in_head, &control->in_tail, g_in_drain,
                SsDrainMetrics{ &metrics->messages_processed, nullptr,
                                &metrics->messages_dropped,
                                &metrics->messages_sequence_gaps },
                MAX_MESSAGES_PER_FRAME,
                [current_ntp](uint32_t sourceId, const uint8_t* payload,
                              uint32_t payload_size, uint32_t seq) -> SsDrainVerdict {
                    // Purge in progress: frames sequenced before the flush
                    // snapshot are stale — consume them undispatched. The
                    // signed delta stays correct across uint32 seq rollover
                    // (pending frames are always far fewer than 2^31 apart).
                    if (g_in_discard_active) {
                        if (static_cast<int32_t>(seq - g_in_discard_below) < 0)
                            return SsDrainVerdict::Consume;
                        g_in_discard_active = false;
                    }

                    // In-place delivery: the payload points into the IN ring
                    // (the consumer owns the region until we return Consume).
                    // scsynth's perform path is synchronous and copies what
                    // it keeps (a scheduled bundle is memcpy'd into the
                    // scheduler's data pool), so nothing retains this pointer.
                    char* osc_buffer = const_cast<char*>(
                        reinterpret_cast<const char*>(payload));
                    const uint8_t* osc = reinterpret_cast<const uint8_t*>(osc_buffer);

                    // Two ways to schedule, one mechanism. A timestamped bundle and
                    // "/schedule <timetag> <blob>" both park OSC for re-dispatch on
                    // time (scheduled_dispatch); everything else dispatches now.
                    // scheduled_dispatch is fail-open — it always consumes the frame
                    // (dropping+counting an un-schedulable one) so a full scheduler
                    // can never head-of-line-block the in-order IN-ring drain.

                    // (1) Bundle. A future timetag → scheduler (synth plane;
                    // SCHED_TAG_SYNTH is protected from the default /sched/flush);
                    // an immediate one (0/1) dispatches now. Either way a bundle is
                    // never a /schedule packet — don't fall through to parse_schedule.
                    if (ss_is_bundle(osc, payload_size)) {
                        uint64_t timetag = ss_bundle_timetag(osc);
                        if (timetag != 0 && timetag != 1) {
                            scheduled_dispatch(osc, payload_size, sourceId,
                                               (int64_t)timetag, SCHED_TAG_SYNTH);
                            return SsDrainVerdict::Consume;
                        }
                    } else {
                        // (2) "/schedule <timetag> <blob>" → scheduler (the inner blob,
                        // re-dispatched on time). SCHED_TAG_DEFAULT — a run-stop flush
                        // cancels it, matching the MIDI/OSC it usually carries.
                        SchedulePacket sp = ss_parse_schedule(osc, payload_size);
                        if (sp.ok) {
                            scheduled_dispatch(sp.blob, sp.blobLen, sourceId,
                                               sp.when, SCHED_TAG_DEFAULT);
                            return SsDrainVerdict::Consume;
                        }
                    }

                    // (3) Everything else → dispatch now. The one address dispatcher
                    // routes it: synth inline (default), control to its handler /
                    // NRT, with no ingress published it goes straight to synth.
                    dispatch(osc, payload_size, sourceId, /*when=*/0, /*blockTime=*/0);
                    return SsDrainVerdict::Consume;
                },
                &stop);

            // The walker resyncs and counts on corruption; policy — rate-
            // limited logging and the status flag — stays the engine's.
            uint32_t gaps_after =
                metrics->messages_sequence_gaps.load(std::memory_order_relaxed);
            if (gaps_after != gaps_before &&
                gap_log_count.load(std::memory_order_relaxed) < 5) {
                ss_log("WARNING: IN sequence gap: %u message(s) missing (total %u)",
                             gaps_after - gaps_before, gaps_after);
                gap_log_count.fetch_add(1, std::memory_order_relaxed);
            }
            if (stop == SsDrainStop::BadMagic || stop == SsDrainStop::BadLength ||
                stop == SsDrainStop::BadCursor) {
                if (corruption_count.load(std::memory_order_relaxed) < 5) {
                    ss_log("ERROR: IN ring corrupt (%s): head=%d tail=%d - pending region dropped",
                                 stop == SsDrainStop::BadMagic  ? "bad magic" :
                                 stop == SsDrainStop::BadLength ? "bad length" : "bad cursor",
                                 control->in_head.load(std::memory_order_relaxed),
                                 control->in_tail.load(std::memory_order_relaxed));
                    corruption_count.fetch_add(1, std::memory_order_relaxed);
                }
                if (stop == SsDrainStop::BadLength)
                    control->status_flags.fetch_or(STATUS_FRAGMENTED_MSG, std::memory_order_relaxed);
            }

#if SUPERSONIC_SYNTH
            // Block size from scsynth's World options. Web: always 128
            // (AudioWorklet render quantum). Native: chosen at boot —
            // typically equal to the hardware callback buffer size.
            const int QUANTUM_SIZE = g_world->mBufLength;

            // Open the render block: zero the output buses (so output channels
            // nothing writes this block are silent) and advance the block counter.
            EngineCore_BeginBlock(g_world);

            // Zero static_audio_bus to prevent accumulation across frames.
            memset(static_audio_bus, 0, QUANTUM_SIZE * g_world->mNumOutputs * sizeof(float));
#endif

            // This block's OSC time window, for draining due scheduled events.
            int64_t currentOscTime = ntp_to_osc_timetag(current_ntp);
            int64_t nextOscTime = currentOscTime + g_osc_increment;

            // Schedule any midi_clock_beat burst ticks due in the look-ahead
            // window (SuperClock-timed) into the same scheduler, so they stay
            // sample-locked to audio.
            if (g_active_superclock.load(std::memory_order_acquire))
                get_midi_clock_out().generate(current_ntp);

            // Fire: drain every event due this block in time order through the
            // shared fire loop, handing each to the SAME dispatch() the immediate
            // drain uses. `ev.when` carries the timetag and currentOscTime is this
            // block's start, so the synth backend places the event sample-accurately
            // (offset = ev.when - block start); other handlers ignore both.
            ss_fire_due(g_scheduler, nextOscTime, currentOscTime,
                [](const uint8_t* d, uint32_t n, uint32_t token, int64_t when, int64_t bt) {
                    dispatch(d, n, token, when, bt);
                });
            // Publish queue depth once per block, after draining (size() reflects
            // released slots — a per-event read would lag release and never reach 0).
            update_scheduler_depth_metric(g_scheduler.size());

#if SUPERSONIC_SYNTH
            // Run the graph (DSP pass): resets the event-time offset, marks the
            // live input buses touched so In.ar reads them (the JS worklet copies
            // input into the input-bus region before this call), and runs scsynth.
            // rt_dsp_guard marks the DSP pass as RT scope — always on, so the
            // suite-wide listener in test_main.cpp reports any global new/delete in
            // the graph pass. Scoped to World_Run (not the whole callback) because
            // that is the region with the hard no-alloc rule; the [rt_alloc] tests
            // add their own guard over the whole process_audio() and assert
            // construction, teardown and steady state allocate nothing.
            {
                rt_alloc::Guard rt_dsp_guard;
                EngineCore_RunBlock(g_world, active_input_channels);
            }

            // Deliver /tr, /n_end, /n_go, etc. produced by this block's graph pass.
            EngineCore_FlushNotifications(g_world);

            // Fast copy audio from g_world->mAudioBus to static_audio_bus
            // Layout: Both buffers are channel-by-channel, 128 samples per channel
            float* src = g_world->mAudioBus;
            float* dst = static_audio_bus;

#ifdef __wasm_simd128__
            // SIMD-optimized copy: process 4 floats at a time
            const int total_samples = g_world->mNumOutputs * QUANTUM_SIZE;
            const int simd_iterations = total_samples / 4;

            for (int i = 0; i < simd_iterations; i++) {
                v128_t vec = wasm_v128_load(src + i * 4);
                wasm_v128_store(dst + i * 4, vec);
            }

            const int remaining = total_samples % 4;
            if (remaining > 0) {
                memcpy(dst + simd_iterations * 4, src + simd_iterations * 4, remaining * sizeof(float));
            }
#else
            memcpy(dst, src, g_world->mNumOutputs * QUANTUM_SIZE * sizeof(float));
#endif
#ifdef __EMSCRIPTEN__
            // Master output tap (slot 0) — WASM only. Native uses the
            // supersonic-audio-out synth (AudioOut2 UGen) to write the
            // same slot from inside the graph; running both would
            // double-write and drop recorded pitch by an octave.
            if (g_shm_audio_buffers) {
                auto* tap = &g_shm_audio_buffers[SHM_AUDIO_MASTER_SLOT];
                if (tap->enabled.load(std::memory_order_relaxed)) {
                    shm_audio_buffer_writer w(tap);
                    const float* channel_data[SHM_AUDIO_CHANNELS];
                    const uint32_t channels = g_world->mNumOutputs;
                    const uint32_t tc = (channels < SHM_AUDIO_CHANNELS) ? channels : SHM_AUDIO_CHANNELS;
                    for (uint32_t c = 0; c < tc; ++c)
                        channel_data[c] = static_audio_bus + c * QUANTUM_SIZE;
                    for (uint32_t c = tc; c < SHM_AUDIO_CHANNELS; ++c)
                        channel_data[c] = channel_data[tc - 1];
                    w.write(channel_data, QUANTUM_SIZE);
                }
            }
#endif // __EMSCRIPTEN__
#endif // SUPERSONIC_SYNTH
        }

        return true; // Keep processor alive
    }

    // Frame a log line as a `/supersonic/debug <text>` OSC message and emit it.
    // Routing keeps the RT-out ring single-writer: on the audio thread the line
    // goes to the lock-free RT-out ring; off the audio thread (watchdog, recovery,
    // boot on native) it goes to the locked multi-producer NRT-out ring, so RT-out
    // never gets a second concurrent writer. The NRT gateway drains and forwards
    // both, so delivery is identical. WASM is single-threaded with no NRT-out
    // drainer, so it always uses RT-out. Either way the line leaves as an ordinary
    // addressed OSC message the host dispatches to its debug channel.
    static void emit_debug_osc(const char* text, uint32_t len) {
        if (!memory_initialized) return;
        if (len > 960) len = 960;  // matches buildDebugOsc's clamp; keeps the metric in sync

        char pkt[1024];
        uint32_t p = supersonic::buildDebugOsc(pkt, text, len);

        // The audio thread owns the lock-free RT-out ring, so it logs there. Any
        // other thread routes to the locked NRT-out ring instead — but only where a
        // backend drains it (g_nrt_egress_drained, published by the native NRT
        // gateway). A single-threaded worklet target (WASM / self-driven device)
        // never drains NRT-out and has no second RT-out writer, so it always uses
        // the always-safe RT-out. Capability signal, not an __EMSCRIPTEN__ branch,
        // so every target shares one codepath.
        const bool useRtOut =
            t_on_audio_thread ||
            !g_nrt_egress_drained.load(std::memory_order_relaxed);
        if (useRtOut) {
            ::ring_buffer_write(
                shared_memory + OUT_BUFFER_START, OUT_BUFFER_SIZE,
                &control->out_head, &control->out_tail, &control->out_sequence,
                EGRESS_BROADCAST_NOTIFY, 0,  // route, source_id (debug broadcasts)
                pkt, p, &control->status_flags);
        } else {
            ss_egress_nrt_write(EGRESS_BROADCAST_NOTIFY, 0,
                                reinterpret_cast<const uint8_t*>(pkt), p);
        }

        // Count the debug line for the metrics view.
        if (metrics) {
            metrics->debug_messages_received.fetch_add(1, std::memory_order_relaxed);
            metrics->debug_bytes_received.fetch_add(len, std::memory_order_relaxed);
        }
    }

    static int ss_log_impl(const char* fmt, va_list args) {
        if (!memory_initialized) return 0;
        char buffer[1024];
        int result = vsnprintf(buffer, sizeof(buffer), fmt, args);
        uint32_t len = 0;
        while (buffer[len] != '\0' && len < sizeof(buffer)) len++;
        emit_debug_osc(buffer, len);
        return result;
    }

    // Variadic version - for direct C++ calls
    extern "C" EMSCRIPTEN_KEEPALIVE
    int ss_log(const char* fmt, ...) {
        va_list args;
        va_start(args, fmt);
        int result = ss_log_impl(fmt, args);
        va_end(args);
        return result;
    }

    // va_list version - for function pointers (matches PrintFunc signature)
    extern "C" EMSCRIPTEN_KEEPALIVE
    int ss_log_va(const char* fmt, va_list args) {
        return ss_log_impl(fmt, args);
    }

    // Raw version - already-formatted string (avoids the vsnprintf double-copy).
    extern "C" EMSCRIPTEN_KEEPALIVE
    int ss_log_raw(const char* msg, uint32_t len) {
        if (!memory_initialized || !msg || len == 0) return 0;
        emit_debug_osc(msg, len);
        return (int)len;
    }

    // Get current metrics
    EMSCRIPTEN_KEEPALIVE
    uint32_t get_process_count() {
        return metrics ? metrics->process_count.load(std::memory_order_relaxed) : 0;
    }

    EMSCRIPTEN_KEEPALIVE
    uint32_t get_messages_processed() {
        return metrics ? metrics->messages_processed.load(std::memory_order_relaxed) : 0;
    }

    EMSCRIPTEN_KEEPALIVE
    uint32_t get_messages_dropped() {
        return metrics ? metrics->messages_dropped.load(std::memory_order_relaxed) : 0;
    }

    EMSCRIPTEN_KEEPALIVE
    uint32_t get_status_flags() {
        return control ? control->status_flags.load(std::memory_order_relaxed) : 0;
    }

    // Audio-bus accessors below dereference the World; they only exist in the
    // synth build. (Their Link-Audio callers — SuperClockNative / SupersonicEngine
    // — are themselves excluded from the no-synth build.)
#if SUPERSONIC_SYNTH
    // Whole audio-bus pool — base + count — so Link Audio sinks/sources
    // can tap arbitrary bus indices the user picks (publishAuxSinks /
    // drainLinkAudioInputsToBuses).
    EMSCRIPTEN_KEEPALIVE
    uintptr_t get_audio_bus_pool() {
        if (!memory_initialized || !g_world) return 0;
        return reinterpret_cast<uintptr_t>(g_world->mAudioBus);
    }

    EMSCRIPTEN_KEEPALIVE
    int get_audio_bus_count() {
        return g_world ? g_world->mNumAudioBusChannels : 0;
    }

    // Index of the first PRIVATE bus. scsynth's bus pool layout is
    // [outputs][inputs][private]; Link Audio inputs must target a
    // private bus. Returns INT_MAX pre-init so any busIdx fails the
    // caller's `>= firstPrivate` check (fail-closed during boot).
    EMSCRIPTEN_KEEPALIVE
    int get_audio_first_private_bus_idx() {
        if (!g_world) return std::numeric_limits<int>::max();
        return g_world->mNumOutputs + g_world->mNumInputs;
    }

    // Mark an audio bus "touched" so In.ar reads it. Callers from
    // INSIDE process_audio (after the per-block mBufCounter++) write
    // the current counter; pre-process_audio callers (e.g. Link Audio
    // drain wired from the audio callback before process_audio runs)
    // write counter+1 so the synth's In.ar check passes in the
    // upcoming block.
    EMSCRIPTEN_KEEPALIVE
    void touch_audio_bus(uint32_t busIdx) {
        if (!g_world || !g_world->mAudioBusTouched) return;
        if (busIdx >= static_cast<uint32_t>(g_world->mNumAudioBusChannels)) return;
        g_world->mAudioBusTouched[busIdx] = g_world->mBufCounter;
    }

    EMSCRIPTEN_KEEPALIVE
    void touch_audio_bus_for_next_block(uint32_t busIdx) {
        if (!g_world || !g_world->mAudioBusTouched) return;
        if (busIdx >= static_cast<uint32_t>(g_world->mNumAudioBusChannels)) return;
        g_world->mAudioBusTouched[busIdx] = g_world->mBufCounter + 1;
    }
#endif // SUPERSONIC_SYNTH

    // scsynth audio output accessor — the master mix staging buffer. In the
    // no-synth build it is never written (no render), so the host reads silence;
    // the symbol stays so lanes.cpp's ss_audio_out() links either way.
    EMSCRIPTEN_KEEPALIVE
    uintptr_t get_audio_output_bus() {
        if (!memory_initialized) {
            return 0;
        }

        return reinterpret_cast<uintptr_t>(static_audio_bus);
    }

    EMSCRIPTEN_KEEPALIVE
    int get_audio_buffer_samples() {
        // Block size currently in use. On web this is always 128
        // (AudioWorklet render quantum). On native it reflects whatever
        // was configured when the World was built. No-synth: the default block.
#if SUPERSONIC_SYNTH
        return g_world ? g_world->mBufLength : sonicpi::kDefaultBlockSize;
#else
        return sonicpi::kDefaultBlockSize;
#endif
    }

    // scsynth audio input accessor
    // Returns pointer to input bus area in mAudioBus (after output buses)
    // Layout: mAudioBus = [output buses][input buses][internal buses].
    // No-synth: no World/input bus, so always 0.
    EMSCRIPTEN_KEEPALIVE
    uintptr_t get_audio_input_bus() {
#if SUPERSONIC_SYNTH
        if (!memory_initialized || !g_world) {
            return 0;
        }

        // Input buses start after output buses in mAudioBus
        // Each bus has mBufLength samples (128)
        return reinterpret_cast<uintptr_t>(
            g_world->mAudioBus + (g_world->mNumOutputs * g_world->mBufLength)
        );
#else
        return 0;
#endif
    }

    // Return version string combining Supersonic and SuperCollider versions
    EMSCRIPTEN_KEEPALIVE
    const char* get_supersonic_version_string() {
        static std::string version;
        if (version.empty()) {
            std::stringstream out;
            out << "Supersonic " << SUPERSONIC_VERSION_MAJOR << "." << SUPERSONIC_VERSION_MINOR
                << "." << SUPERSONIC_VERSION_PATCH << " (SuperCollider " << SC_VersionString() << ")";
            version = out.str();
        }
        return version.c_str();
    }

    // Return the time conversion offset (NTP seconds when AudioContext was 0)
    // JavaScript can use this to convert AudioContext time to Unix time
    EMSCRIPTEN_KEEPALIVE
    double get_time_offset() {
        return g_time_zero_osc;
    }

} // namespace scsynth

// ============================================================================
// RING BUFFER HELPER FUNCTIONS (outside namespace for C++ linkage)
// ============================================================================

// Lock-free SPSC ring writer for the OUT egress ring — scsynth replies, the
// sample loader's /done|/fail, and /supersonic/debug. All three run on the
// audio thread, so OUT has a single producer and needs no write lock.
//
// Same wire convention as every ring (RingBufferWriter.h / the JS writer):
// frames never wrap — when a frame won't fit before the end, a PADDING_MAGIC
// marker fills the remainder and the write restarts at offset 0, so readers
// parse in place. Overflow drops the message (false) and counts it.
//
// All state is passed in (head/tail, sequence counter, status-flags word,
// metrics) rather than read from scsynth globals, so the function has no hidden
// dependencies and is unit-tested directly (test_ring_buffer_write.cpp).
bool ring_buffer_write(
    uint8_t* buffer_start,
    uint32_t buffer_size,
    std::atomic<int32_t>* head,
    std::atomic<int32_t>* tail,
    std::atomic<int32_t>* sequence,
    uint32_t route,
    uint32_t source_id,
    const void* data,
    uint32_t data_size,
    std::atomic<uint32_t>* status_flags,
    PerformanceMetrics* metrics
) {
    // Egress frame: Message{sourceId = token} + [route:u32][osc]; the route word
    // counts toward the message length.
    Message header;
    header.magic = MESSAGE_MAGIC;
    header.length = sizeof(Message) + static_cast<uint32_t>(sizeof(uint32_t)) + data_size;
    header.sequence = static_cast<uint32_t>(sequence->fetch_add(1, std::memory_order_relaxed));
    header.sourceId = source_id;

    // Load head and tail with acquire semantics
    int32_t current_head = head->load(std::memory_order_acquire);
    int32_t current_tail = tail->load(std::memory_order_acquire);

    // Frame footprint: header.length is exact; occupancy and cursor advance
    // are its 4-byte-aligned rounding, matching RingBufferWriter.h and the JS
    // writer (readers advance the tail by this footprint).
    const uint32_t footprint = (header.length + 3u) & ~3u;

    // Calculate available space in the buffer
    uint32_t available = (buffer_size - 1 - current_head + current_tail) % buffer_size;

    // Check if there's enough space for the message
    if (available < footprint) {
        // Not enough space - drop the message and track metrics
        if (metrics) metrics->messages_dropped.fetch_add(1, std::memory_order_relaxed);
        if (status_flags) status_flags->fetch_or(STATUS_BUFFER_FULL, std::memory_order_relaxed);
        return false;
    }

    // Check if message fits contiguously, otherwise write padding and wrap to 0.
    //
    // After wrapping, we must re-check that there's enough space between position 0
    // and tail — the initial available-space check included bytes that will be wasted
    // as padding, so it can overestimate the usable space at the front.
    uint32_t space_to_end = buffer_size - current_head;
    if (footprint > space_to_end) {
        // Verify space at front after wrap (tail-1 to avoid head==tail ambiguity)
        uint32_t space_at_front = (current_tail > 0) ? (current_tail - 1) : 0;
        if (space_at_front < footprint) {
            if (metrics) metrics->messages_dropped.fetch_add(1, std::memory_order_relaxed);
            if (status_flags) status_flags->fetch_or(STATUS_BUFFER_FULL, std::memory_order_relaxed);
            return false;
        }

        // Padding marker: magic word, zeros to the end of the ring (matching
        // the other writers — when >= 16 bytes remain this doubles as a full
        // zeroed pad header; 4-byte alignment guarantees the magic fits).
        uint32_t pad = PADDING_MAGIC;
        std::memcpy(buffer_start + current_head, &pad, sizeof(pad));
        if (space_to_end > sizeof(pad)) {
            std::memset(buffer_start + current_head + sizeof(pad), 0,
                        space_to_end - sizeof(pad));
        }

        // Wrap head to beginning
        current_head = 0;
    }

    // Write message header (now contiguous)
    std::memcpy(buffer_start + current_head, &header, sizeof(Message));

    std::memcpy(buffer_start + current_head + sizeof(Message), &route, sizeof(uint32_t));
    std::memcpy(buffer_start + current_head + sizeof(Message) + sizeof(uint32_t), data, data_size);

    // Zero the 0-3 alignment pad bytes (determinism: no stale ring bytes
    // inside a frame's footprint).
    if (footprint > header.length) {
        std::memset(buffer_start + current_head + header.length, 0,
                    footprint - header.length);
    }

    // Update head pointer with release semantics (publish message)
    int32_t new_head = (current_head + footprint) % buffer_size;
    head->store(new_head, std::memory_order_release);

    // Track peak buffer usage at write time — the reader may drain the
    // buffer before the periodic metrics sampling sees the fill level.
    if (metrics) {
        uint32_t used = (new_head - current_tail + buffer_size) % buffer_size;
        uint32_t prev = metrics->out_buffer_peak_bytes.load(std::memory_order_relaxed);
        while (used > prev) {
            if (metrics->out_buffer_peak_bytes.compare_exchange_weak(
                    prev, used, std::memory_order_relaxed))
                break;
        }
    }

    return true;
}

// OSC reply callback for scsynth
// This is called by scsynth when it needs to send OSC replies (e.g., /done, /n_go, etc.)
// ReplyAddress is a scsynth type, so the reply callback is part of the synth build.
#if SUPERSONIC_SYNTH
void osc_reply_to_ring_buffer(ReplyAddress* addr, char* msg, int size) {
    using namespace scsynth;  // Access globals from scsynth namespace

    if (!control || !shared_memory) {
        return;
    }

    // Route the reply by the origin token carried in mReplyData: non-zero → reply
    // to that specific client; 0 → broadcast to the notify audience (engine-
    // originated replies, and notifications whose subscriber address carries no
    // token). The egress drain resolves the token via the transport.
    uint32_t token = addr ? static_cast<uint32_t>(reinterpret_cast<uintptr_t>(addr->mReplyData)) : 0;
    uint32_t route = token ? EGRESS_REPLY : EGRESS_BROADCAST_NOTIFY;

    // Use unified ring buffer write with full protection.
    ring_buffer_write(
        shared_memory + OUT_BUFFER_START,  // ring base
        OUT_BUFFER_SIZE,                   // buffer_size
        &control->out_head,                // head
        &control->out_tail,                // tail
        &control->out_sequence,            // sequence
        route,                             // route (REPLY to token, or broadcast)
        token,                             // source_id (origin token; 0 = broadcast)
        msg,                               // data
        size,                              // data_size
        &control->status_flags,            // status_flags
        metrics                            // metrics
    );
}
#endif // SUPERSONIC_SYNTH

// ── Unified scheduler bridge (global linkage; see EngineScheduler.h) ──────────

EngineScheduler& get_scheduler() {
    using namespace scsynth;
    return g_scheduler;
}

extern "C" void ss_defer_schedule(double ntp_seconds, uint32_t tag, uint32_t origin,
                                  const uint8_t* osc, uint32_t len) {
    using namespace scsynth;
    if (!g_scheduler.addScheduled(ntp_to_osc_timetag(ntp_seconds), tag, origin, osc, len))
        increment_scheduler_drop_metric();   // full/oversize — surface it, don't lose it silently
}

// Set the synth plane's sub-sample offset for a SCHEDULED message and surface
// lateness in the metrics. `when` is the message's OSC timetag and `blockTime`
// this block's start — both from the call ctx (threaded by the fire loop, no
// global). Owned by the synth backend — the only handler that reads them; the
// generic dispatcher and every other backend ignore them. Synth-only: the
// no-synth core registers no default route, so this is never called or linked.
#if SUPERSONIC_SYNTH
//
// Immediate messages (the OSC sentinels 0/1) leave the offset untouched: only
// timed events set a sub-block offset, and the graph's DSP pass resets it
// afterwards. (Forcing it to 0 would clobber a still-live scheduled offset that a
// synth created this block reads on its first run — silently breaking accuracy.)
//
// The check is `== 0 || == 1`, NOT `<= 1`: a present-day OSC timetag is huge and,
// taken as int64, has its sign bit set — i.e. it is NEGATIVE. `<= 1` would
// misclassify every real scheduled event as immediate and skip its offset.
static void synth_apply_offset(int64_t when, int64_t blockTime) {
    if (when == 0 || when == 1) return;   // OSC "immediate" sentinels only
    float diffTime = (float)(when - blockTime) * g_osc_to_samples + 0.5f;
    float diffTimeFloor = floorf(diffTime);
    g_world->mSampleOffset = (int)diffTimeFloor;
    g_world->mSubsampleOffset = diffTime - diffTimeFloor;
    if (g_world->mSampleOffset < 0)
        g_world->mSampleOffset = 0;
    else if (g_world->mSampleOffset >= g_world->mBufLength)
        g_world->mSampleOffset = g_world->mBufLength - 1;

    // Lateness: only messages older than a full quantum are genuinely late
    // (sub-quantum arrivals just don't align with quantum boundaries and run at
    // the correct sub-sample offset above).
    if (!metrics) return;
    double time_diff_ms = ((double)(when - blockTime) / 4294967296.0) * 1000.0;
    double quantum_ms = (1000.0 * g_world->mBufLength) / g_world->mSampleRate;
    if (time_diff_ms >= -quantum_ms) return;
    double raw_late_ms = -time_diff_ms;
    int32_t late_ms = (raw_late_ms > 10000.0) ? 10000 : (int32_t)raw_late_ms;
    int late_now = late_count.fetch_add(1, std::memory_order_relaxed) + 1;
    metrics->scheduler_lates.fetch_add(1, std::memory_order_relaxed);
    int32_t current_max = metrics->scheduler_max_late_ms.load(std::memory_order_relaxed);
    while (late_ms > current_max) {
        if (metrics->scheduler_max_late_ms.compare_exchange_weak(
                current_max, late_ms, std::memory_order_relaxed, std::memory_order_relaxed))
            break;
    }
    metrics->scheduler_last_late_ms.store(late_ms, std::memory_order_relaxed);
    metrics->scheduler_last_late_tick.store(
        metrics->process_count.load(std::memory_order_relaxed), std::memory_order_relaxed);
    // Count-based sampling alone hides lates 2..99, so a burst of late
    // events can leave no log trace. Keep the count milestones but also emit
    // at most one line per second of block time.
    static int64_t last_late_log_osc = 0;
    const bool second_elapsed =
        (blockTime - last_late_log_osc) >= (int64_t)4294967296LL;
    if (late_now == 1 || late_now % 100 == 0 || second_elapsed) {
        last_late_log_osc = blockTime;
        ss_log("LATE: %.1fms (count=%d)", -time_diff_ms, late_now);
    }
}

bool ss_synth_default_route(void* /*routeCtx*/, const void* callCtx,
                            const uint8_t* data, std::size_t len) {
    using namespace scsynth;
    if (!g_world) return true;
    auto* cc = static_cast<const DrainCallCtx*>(callCtx);
    ReplyAddress reply = ring_reply(cc ? cc->sourceId : 0);  // built from the threaded token
    synth_apply_offset(cc ? cc->when : 0, cc ? cc->blockTime : 0);
    char* osc = reinterpret_cast<char*>(const_cast<uint8_t*>(data));
    if (ss_is_bundle(data, static_cast<uint32_t>(len))) {
        OSC_Packet packet;
        packet.mData      = osc;
        packet.mSize      = static_cast<int>(len);
        packet.mIsBundle  = true;
        packet.mReplyAddr = reply;
        PerformOSCBundle(g_world, &packet);
    } else {
        PerformOSCMessage(g_world, static_cast<int>(len), osc, &reply);
    }
    return true;
}
#endif // SUPERSONIC_SYNTH
