/*
    SuperSonic - SuperCollider AudioWorklet WebAssembly port
    Copyright (c) 2025 Sam Aaron

    Based on SuperCollider by James McCartney and community
    GPL v3 or later
*/

#include "audio_processor.h"
#include "audio_config.h"
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

// scsynth includes
#include "SC_World.h"              // from scsynth/include/plugin_interface/
#include "SC_WorldOptions.h"       // from scsynth/include/server/
#include "SC_Prototypes.h"         // from scsynth/server/
#include "OSC_Packet.h"            // from scsynth/server/
#include "SC_Reply.h"              // from scsynth/include/common/
#include "SC_ReplyImpl.hpp"        // from scsynth/common/ - for ReplyAddress
#include "SC_GraphDef.h"           // from scsynth/server/
#include "SC_Graph.h"              // from scsynth/server/
#include "SC_Group.h"              // from scsynth/server/
#include "SC_HiddenWorld.h"        // from scsynth/server/
#include "sc_msg_iter.h"           // from scsynth/include/plugin_interface/
#include "Samp.hpp"                // for sine table initialization

// Scheduler includes
#include "scheduler/BundleScheduler.h"

// Node tree for SharedArrayBuffer polling
#include "node_tree.h"

// Pre-allocated heap for RT-safe allocations
#include "supersonic_heap.h"
#include "supersonic_config.h"

// Thread-local RT guard for allocation detection (read by test binary only)
#include "rt_alloc.h"

// Definition of the slot-array pointer declared in shm_audio_buffer.hpp.
// Assigned once at init; AudioOut2 instances read it to locate their slot.
shm_audio_buffer* g_shm_audio_buffers = nullptr;

// Forward declarations
int PerformOSCMessage(World* inWorld, int inSize, char* inData, ReplyAddress* inReply);
void PerformOSCBundle(World* inWorld, OSC_Packet* inPacket);

// Forward declare UnrollOSCPacket from SC_ComPort.cpp (in scsynth namespace)
namespace scsynth {
    bool UnrollOSCPacket(World* inWorld, int inSize, char* inData, OSC_Packet* inPacket);
}

// Forward declare ring buffer write function (defined after namespace)
bool ring_buffer_write(
    uint8_t* buffer_start,
    uint32_t buffer_size,
    uint32_t buffer_start_offset,
    std::atomic<int32_t>* head,
    std::atomic<int32_t>* tail,
    const void* data,
    uint32_t data_size,
    PerformanceMetrics* metrics
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
#include "scsynth/common/SC_Version.hpp"

// Supersonic version — defined in supersonic_config.h (single source of truth)

// Global pointers
extern "C" {
    // Static ring buffer in WASM data segment, separate from scsynth heap.
    // Size: ~1.4MB (IN: 768KB, OUT: 128KB, DEBUG: 64KB, control/metrics, node tree ~57KB, audio capture ~375KB)
    // 16-byte aligned for shm_audio_buffer slots (alignas(16)) at
    // SHM_AUDIO_START; JS Float64Array access only needs 8.
    alignas(16) uint8_t ring_buffer_storage[TOTAL_BUFFER_SIZE];

    // Validate at compile time that buffer layout fits in allocated storage
    static_assert(TOTAL_BUFFER_SIZE <= sizeof(ring_buffer_storage),
                  "Buffer layout exceeds allocated storage!");

    // Pre-allocated audio bus staging buffer — sized to the compile-time
    // max so runtime block sizes up to that cap fit without reallocation.
    // (128 * 128 * 4 = 64 KB on web, up to 512 KB on native.) Used as a
    // scratch copy area for channel-major output; we only write
    // g_world->mBufLength samples per channel at runtime.
    alignas(16) float static_audio_bus[sonicpi::kMaxBlockSize * sonicpi::kMaxChannels];

    // Static OSC message buffer - MUST NOT be on stack!
    // MAX_MESSAGE_SIZE is ~768KB which would overflow the WASM stack.
    // This buffer is used to copy OSC messages from ring buffer before processing.
    alignas(8) char static_osc_buffer[MAX_MESSAGE_SIZE];

    void* g_rt_pool_ptr = nullptr;
    size_t g_rt_pool_size = 0;

    // Optional override for the metrics pointer. When set (native backend with
    // a public POSIX shm segment), init_memory() uses this address instead of
    // the in-band slot inside ring_buffer_storage. Lets Sonic Pi observe
    // metrics directly via the public shared segment without OSC roundtrips.
    PerformanceMetrics* g_external_metrics = nullptr;

    // Mirror of g_external_metrics for the audio-buffer slot array. When
    // non-null (native, cross-process shm), the post-block hook and any
    // AudioOut2 UGens write into this segment-resident array so external
    // readers (e.g. the Sonic Pi GUI recorder) see the frames. On WASM
    // and no-shm native init_memory() falls back to the in-band slots
    // inside ring_buffer_storage.
    shm_audio_buffer* g_external_audio_buffers = nullptr;

    uint8_t* shared_memory = nullptr;
    ControlPointers* control = nullptr;
    PerformanceMetrics* metrics = nullptr;
    double* ntp_start_time = nullptr;        // NEW
    std::atomic<int32_t>* drift_offset = nullptr;  // NEW
    std::atomic<int32_t>* global_offset = nullptr; // NEW
    bool memory_initialized = false;
    World* g_world = nullptr;

    // OSC Bundle Scheduler - Index-based pool for RT-safety
    // Events stored in pool (never copied), queue only stores small indices
    BundleScheduler g_scheduler;

    // File-scope state shared across threads: written by clear_scheduler()
    // on the control thread, read/updated by process_audio() on the audio
    // thread. Relaxed ordering — these are diagnostic counters with no
    // cross-variable invariants.
    std::atomic<int32_t> last_in_sequence{-1};
    std::atomic<uint32_t> local_in_peak{0};
    std::atomic<uint32_t> local_out_peak{0};
    std::atomic<uint32_t> local_debug_peak{0};
    std::atomic<uint32_t> metrics_cycle{0};
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
        worklet_debug("Time offset set from JavaScript: %.6f", offset);
    }

    // Convert NTP double (seconds since 1900) to OSC timetag (int64)
    inline int64_t ntp_to_osc_timetag(double ntp) {
        uint32_t s = static_cast<uint32_t>(ntp);
        uint32_t f = static_cast<uint32_t>((ntp - s) * 4294967296.0);
        return static_cast<int64_t>((static_cast<uint64_t>(s) << 32) | f);
    }

    // Helper: Check if OSC data is a bundle
    bool is_bundle(const char* data, uint32_t size) {
        if (size < 16) return false;  // Minimum bundle size
        return strncmp(data, "#bundle", 7) == 0;
    }

    // Helper: Extract NTP timetag from bundle (8 bytes at offset 8)
    uint64_t extract_timetag(const char* bundle_data) {
        uint64_t timetag = 0;
        for (int i = 0; i < 8; i++) {
            timetag = (timetag << 8) | (uint8_t)bundle_data[8 + i];
        }
        return timetag;
    }

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

    // Clear the WASM-side bundle scheduler.
    // Called from the worklet JS layer (via postMessage flag) to flush
    // all pending scheduled bundles without going through the ring buffer.
    //
    // Note: the IN ring buffer is drained separately by the JS worklet in its
    // message handler (immediately on receiving clearSched), not here. Draining
    // eagerly in JS ensures stale messages are discarded before the ack is sent,
    // so new messages written after purge() resolves are not affected.
    EMSCRIPTEN_KEEPALIVE
    void clear_scheduler() {
        last_in_sequence.store(-1, std::memory_order_relaxed);
        local_in_peak.store(0, std::memory_order_relaxed);
        local_out_peak.store(0, std::memory_order_relaxed);
        local_debug_peak.store(0, std::memory_order_relaxed);
        metrics_cycle.store(0, std::memory_order_relaxed);
        corruption_count.store(0, std::memory_order_relaxed);
        gap_log_count.store(0, std::memory_order_relaxed);
        late_count.store(0, std::memory_order_relaxed);
        update_scheduler_depth_metric(0);
        g_scheduler.RequestClear();
    }

    // RT-safe bundle scheduling - no malloc!
    // Returns true if scheduled, false if queue full or data pool exhausted.
    // Variable-size: bundles up to the pool's free space are accepted.
    bool schedule_bundle(World* world, int64_t ntp_time, int64_t current_osc_time,
                        const char* data, int32_t size, const ReplyAddress& reply_addr) {
        if (!g_scheduler.Add(world, ntp_time, data, size, reply_addr)) {
            worklet_debug("ERROR: Scheduler full — queue=%d pool=%u/%u bytes, bundle=%d bytes",
                         g_scheduler.Size(), g_scheduler.DataPoolUsed(),
                         g_scheduler.DataPoolCapacity(), size);
            increment_scheduler_drop_metric();
            update_scheduler_depth_metric(g_scheduler.Size());
            return false;
        }

        update_scheduler_depth_metric(g_scheduler.Size());

        return true;
    }

    // Initialize memory pointers using the static ring buffer
    EMSCRIPTEN_KEEPALIVE
    void init_memory(double sample_rate) {
        shared_memory = ring_buffer_storage;
        control = reinterpret_cast<ControlPointers*>(shared_memory + CONTROL_START);
        // Metrics: prefer the external-shm slot when one was set by the host
        // (native engine with public POSIX shm segment). Falls back to the
        // in-band slot inside ring_buffer_storage on web (and on native if no
        // shm creator was constructed, e.g. headless tests with udpPort = 0).
        metrics = g_external_metrics
                ? g_external_metrics
                : reinterpret_cast<PerformanceMetrics*>(shared_memory + METRICS_START);

        // Timing pointers
        ntp_start_time = reinterpret_cast<double*>(shared_memory + NTP_START_TIME_START);
        drift_offset = reinterpret_cast<std::atomic<int32_t>*>(shared_memory + DRIFT_OFFSET_START);
        global_offset = reinterpret_cast<std::atomic<int32_t>*>(shared_memory + GLOBAL_OFFSET_START);

        // Initialize timing (NTP_START_TIME is write-once from JavaScript, don't touch it)
        // *ntp_start_time is written by JavaScript after AudioContext starts
        drift_offset->store(0, std::memory_order_relaxed);
        global_offset->store(0, std::memory_order_relaxed);

        // Initialize all atomics to 0
        control->in_head.store(0, std::memory_order_relaxed);
        control->in_tail.store(0, std::memory_order_relaxed);
        control->out_head.store(0, std::memory_order_relaxed);
        control->out_tail.store(0, std::memory_order_relaxed);
        control->debug_head.store(0, std::memory_order_relaxed);
        control->debug_tail.store(0, std::memory_order_relaxed);
        control->in_sequence.store(0, std::memory_order_relaxed);
        control->out_sequence.store(0, std::memory_order_relaxed);
        control->debug_sequence.store(0, std::memory_order_relaxed);
        control->status_flags.store(STATUS_OK, std::memory_order_relaxed);
        control->in_write_lock.store(0, std::memory_order_relaxed);  // 0 = unlocked

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

        // Initialize free list and hash table for O(1) node tree operations
        NodeTree_InitIndices();

        worklet_debug("[NodeTree] Initialized at offset %u, size %u bytes",
                     NODE_TREE_START, NODE_TREE_SIZE);

        // Audio buffer slot array. Slot 0 carries the master output mix
        // and is written by the post-block hook below when `enabled` is
        // set; slots 1..N-1 are written by AudioOut2 UGens (each calls
        // activate() from its Ctor). Native cross-process mode supplies
        // an override into the POSIX shm segment so external readers see
        // the writes; otherwise the slots live in ring_buffer_storage.
        shm_audio_buffer* slots = g_external_audio_buffers
            ? g_external_audio_buffers
            : reinterpret_cast<shm_audio_buffer*>(
                  shared_memory + SHM_AUDIO_START);
        memset(static_cast<void*>(slots), 0,
               MAX_SHM_AUDIO_BUFFERS * sizeof(shm_audio_buffer));
        slots[SHM_AUDIO_MASTER_SLOT].sample_rate = static_cast<uint32_t>(sample_rate);
        slots[SHM_AUDIO_MASTER_SLOT].channels = SHM_AUDIO_CHANNELS;
        slots[SHM_AUDIO_MASTER_SLOT].capacity_frames = SHM_AUDIO_FRAMES;
        g_shm_audio_buffers = slots;

        // Initialize scope buffers
        {
            uint8_t* scopeBase = shared_memory + SHM_SCOPE_START;
            memset(scopeBase, 0, SHM_SCOPE_TOTAL_SIZE);
            auto* maxScopes = reinterpret_cast<uint32_t*>(scopeBase + 0);
            auto* framesPerScope = reinterpret_cast<uint32_t*>(scopeBase + 8);
            *maxScopes = SHM_SCOPE_MAX_SCOPES;
            *framesPerScope = SHM_SCOPE_FRAMES_PER_SCOPE;
        }

        // Enable worklet_debug
        memory_initialized = true;

        // Boot message shown after ASCII art below

        // Read worldOptions from ring_buffer_storage at WORLD_OPTIONS_START.
        // This offset is outside the ring buffers, so OSC traffic cannot overwrite it.
        uint32_t* worldOptionsPtr = (uint32_t*)((uint8_t*)ring_buffer_storage + WORLD_OPTIONS_START);

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
            uint32_t rtPoolOffset = worldOptionsPtr[16];
            uint32_t rtPoolBytes = options.mRealTimeMemorySize * 1024;
            if (rtPoolOffset > 0 && rtPoolBytes > 0) {
                g_rt_pool_ptr = (void*)(shared_memory + rtPoolOffset);
                g_rt_pool_size = rtPoolBytes;
                memset(g_rt_pool_ptr, 0, g_rt_pool_size);
                worklet_debug("RT_POOL: pre-allocated at offset %u (%uMB) size %uMB",
                    rtPoolOffset, rtPoolOffset / (1024*1024), rtPoolBytes / (1024*1024));
            }
        }
#endif
#ifndef __EMSCRIPTEN__
        options.mSharedMemoryID = worldOptionsPtr[18];              // UDP port for boost shm (native only)
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
                worklet_debug("FATAL: WASM heap (sbrk=0x%x) overlaps RT pool (start=0x%x) — reduce heap usage or increase rtPoolOffset",
                    (uint32_t)heap_end, (uint32_t)rt_start);
                control->status_flags.fetch_or(STATUS_WASM_ERROR, std::memory_order_relaxed);
                return;
            }
            worklet_debug("MEMORY OK: heap<0x%x rt=[0x%x,0x%x) buf=0x%x+",
                (uint32_t)heap_end, (uint32_t)rt_start, (uint32_t)rt_end, (uint32_t)rt_end);
        }
#endif

        // Initialize pre-allocated heap (no-op on WASM, creates AllocPool on native)
        supersonic_heap_init(SUPERSONIC_HEAP_SIZE);

        // Create World
        try {
            g_world = World_New(&options);
        } catch (const std::exception& e) {
            worklet_debug("ERROR: World_New threw exception: %s", e.what());
            control->status_flags.fetch_or(STATUS_WASM_ERROR, std::memory_order_relaxed);
            return;
        } catch (...) {
            worklet_debug("ERROR: World_New threw unknown exception");
            control->status_flags.fetch_or(STATUS_WASM_ERROR, std::memory_order_relaxed);
            return;
        }

        if (!g_world) {
            worklet_debug("ERROR: Failed to create World");
            control->status_flags.fetch_or(STATUS_WASM_ERROR, std::memory_order_relaxed);
            return;
        }

        // Initialize sample rate and rates (FullRate, BufRate)
        World_SetSampleRate(g_world, sample_rate);

        if (!g_world->mAudioBusTouched) {
            worklet_debug("ERROR: mAudioBusTouched is NULL");
            control->status_flags.fetch_or(STATUS_WASM_ERROR, std::memory_order_relaxed);
            return;
        }

        if (!g_world->mControlBusTouched) {
            worklet_debug("ERROR: mControlBusTouched is NULL");
            control->status_flags.fetch_or(STATUS_WASM_ERROR, std::memory_order_relaxed);
            return;
        }

        // Zero the static audio bus
        memset(static_audio_bus, 0, sizeof(static_audio_bus));

        // Start the World (equivalent to server.boot() in SuperCollider)
        if (!g_world->mAudioBusTouched || !g_world->mControlBusTouched) {
            worklet_debug("ERROR: NULL pointer before World_Start");
            control->status_flags.fetch_or(STATUS_WASM_ERROR, std::memory_order_relaxed);
            return;
        }

        World_Start(g_world);

        // Verify critical allocations succeeded
        if (!g_world->hw->mWireBufSpace) {
            worklet_debug("ERROR: Wire buffer allocation failed");
            control->status_flags.fetch_or(STATUS_WASM_ERROR, std::memory_order_relaxed);
            return;
        }

        // Initialize scheduler time constants (from SC_CoreAudio.cpp:426-428)
        int buf_length = g_world->mBufLength;  // 128 samples
        g_osc_increment_numerator = (double)buf_length;
        g_osc_increment = (int64_t)(g_osc_increment_numerator / sample_rate * 4294967296.0);
        g_osc_to_samples = sample_rate / 4294967296.0;

        // Clear scheduler
        g_scheduler.Clear();
        update_scheduler_depth_metric(0);

        // Add root group to node tree (it was created during World_New but doesn't trigger Node_StateMsg)
        if (g_world->mTopGroup) {
            uint8_t* node_tree_ptr = shared_memory + NODE_TREE_START;
            NodeTreeHeader* tree_header = reinterpret_cast<NodeTreeHeader*>(node_tree_ptr);
            NodeEntry* tree_entries = reinterpret_cast<NodeEntry*>(node_tree_ptr + NODE_TREE_HEADER_SIZE);
            NodeTree_Add(&g_world->mTopGroup->mNode, tree_header, tree_entries);
        }


#ifdef __EMSCRIPTEN__
        worklet_debug(R"(
░█▀▀░█░█░█▀█░█▀▀░█▀▄░█▀▀░█▀█░█▀█░▀█▀░█▀▀
░▀▀█░█░█░█▀▀░█▀▀░█▀▄░▀▀█░█░█░█░█░░█░░█░░
░▀▀▀░▀▀▀░▀░░░▀▀▀░▀░▀░▀▀▀░▀▀▀░▀░▀░▀▀▀░▀▀▀)");
        worklet_debug("v%d.%d.%d (scsynth %d.%d.%d)",
                     SUPERSONIC_VERSION_MAJOR, SUPERSONIC_VERSION_MINOR, SUPERSONIC_VERSION_PATCH,
                     SC_VersionMajor, SC_VersionMinor, SC_VersionPatch);
        {
            const char* transport_mode = worldOptionsPtr[17] ? "PM" : "SAB";
            worklet_debug("%.0fkHz %dch [%s]",
                         sample_rate / 1000, options.mNumOutputBusChannels, transport_mode);
        }
        worklet_debug("");
        worklet_debug("> scsynth ready...");
#endif
    }

#ifndef __EMSCRIPTEN__
    // destroy_world / rebuild_world — for native cold swap (device sample rate change).
    // Tears down the World (keeping UGen plugins loaded) and rebuilds with new sample rate.
    // Cold-swap entry/exit — the engine's state-transition lines
    // ([supersonic] state: restarting -> running) already record when these
    // fire, so separate tracing here is redundant.
    void destroy_world() {
        if (g_world) {
            World_Cleanup(g_world, false);  // false = keep UGen plugins loaded
            g_world = nullptr;
        }
        supersonic_heap_destroy();
        g_scheduler.Clear();
        update_scheduler_depth_metric(0);
        last_in_sequence.store(-1, std::memory_order_relaxed);
    }

    void rebuild_world(double sample_rate) {
        // Re-read worldOptions from ring_buffer_storage + WORLD_OPTIONS_START
        // (caller must update opts[WorldOpts::kSampleRate] before calling)
        init_memory(sample_rate);
    }
#endif

    // Main audio processing function - called every audio frame (128 samples)
    // current_time: AudioContext.currentTime
    // active_output_channels: Number of output channels from AudioContext
    // active_input_channels: Number of input channels from AudioContext
    EMSCRIPTEN_KEEPALIVE
    bool process_audio(double current_time, uint32_t active_output_channels, uint32_t active_input_channels) {
        if (!memory_initialized || !g_world) {
            return true; // Not ready or world destroyed during cold swap — output silence
        }

        if (!metrics) {
            return false;
        }

        g_scheduler.DrainPendingClear();

        // Calculate current NTP time from components
        // currentNTP = audioContextTime + ntp_start + (drift_us/1000000) + (global_ms/1000)
        // Read ntp_start_time directly from shared memory every frame
        // (no caching - ensures immediate response to timing resync after resume)
        // Read drift with acquire — this fences the non-atomic ntp_start_time read below.
        // JS writes ntp_start_time (float64, non-atomic) then does Atomics.store on drift
        // (release). Our acquire here ensures we see the complete float64.
        double drift_seconds = drift_offset ? (drift_offset->load(std::memory_order_acquire) / 1000000.0) : 0.0;
        double ntp_start = (ntp_start_time && *ntp_start_time != 0.0) ? *ntp_start_time : 0.0;
        double global_seconds = global_offset ? (global_offset->load(std::memory_order_relaxed) / 1000.0) : 0.0;

        double current_ntp = current_time + ntp_start + drift_seconds + global_seconds;

        metrics->process_count.fetch_add(1, std::memory_order_relaxed);

        // Calculate and write ring buffer usage to metrics BEFORE consuming messages
        // so the metric reflects actual queue depth as seen by the audio thread
        {
            int32_t in_head = control->in_head.load(std::memory_order_relaxed);
            int32_t in_tail = control->in_tail.load(std::memory_order_relaxed);
            uint32_t in_used = (in_head - in_tail + IN_BUFFER_SIZE) % IN_BUFFER_SIZE;
            metrics->in_buffer_used_bytes.store(in_used, std::memory_order_relaxed);
            if (in_used > local_in_peak.load(std::memory_order_relaxed)) {
                local_in_peak.store(in_used, std::memory_order_relaxed);
            }

            int32_t out_head = control->out_head.load(std::memory_order_relaxed);
            int32_t out_tail = control->out_tail.load(std::memory_order_relaxed);
            uint32_t out_used = (out_head - out_tail + OUT_BUFFER_SIZE) % OUT_BUFFER_SIZE;
            metrics->out_buffer_used_bytes.store(out_used, std::memory_order_relaxed);
            if (out_used > local_out_peak.load(std::memory_order_relaxed)) {
                local_out_peak.store(out_used, std::memory_order_relaxed);
            }

            int32_t debug_head = control->debug_head.load(std::memory_order_relaxed);
            int32_t debug_tail = control->debug_tail.load(std::memory_order_relaxed);
            uint32_t debug_used = (debug_head - debug_tail + DEBUG_BUFFER_SIZE) % DEBUG_BUFFER_SIZE;
            metrics->debug_buffer_used_bytes.store(debug_used, std::memory_order_relaxed);
            if (debug_used > local_debug_peak.load(std::memory_order_relaxed)) {
                local_debug_peak.store(debug_used, std::memory_order_relaxed);
            }

            // Write peaks to atomics every 16 cycles (~43ms at 48kHz/128)
            if (metrics_cycle.fetch_add(1, std::memory_order_relaxed) + 1 >= 16) {
                metrics_cycle.store(0, std::memory_order_relaxed);
                metrics->in_buffer_peak_bytes.store(local_in_peak.load(std::memory_order_relaxed), std::memory_order_relaxed);
                metrics->out_buffer_peak_bytes.store(local_out_peak.load(std::memory_order_relaxed), std::memory_order_relaxed);
                metrics->debug_buffer_peak_bytes.store(local_debug_peak.load(std::memory_order_relaxed), std::memory_order_relaxed);
            }
        }

        // Process incoming OSC messages if scsynth is ready
        if (g_world) {
            int32_t in_head = control->in_head.load(std::memory_order_acquire);
            int32_t in_tail = control->in_tail.load(std::memory_order_acquire);


            // Process available messages (limit per frame to stay within audio budget)
            const int MAX_MESSAGES_PER_FRAME = 32;
            int messages_this_frame = 0;
            while (in_head != in_tail && messages_this_frame < MAX_MESSAGES_PER_FRAME) {
                uint32_t msg_offset = IN_BUFFER_START + in_tail;
                uint32_t space_to_end = IN_BUFFER_SIZE - in_tail;

                // ringbuf.js approach: no padding markers, handle split reads
                // Read message header (may be split across wrap boundary)
                Message header;

                if (space_to_end >= sizeof(Message)) {
                    // Header fits contiguously
                    std::memcpy(&header, shared_memory + msg_offset, sizeof(Message));
                } else {
                    // Header is split - read in two parts
                    std::memcpy(&header, shared_memory + msg_offset, space_to_end);
                    std::memcpy((char*)&header + space_to_end, shared_memory + IN_BUFFER_START, sizeof(Message) - space_to_end);
                }

                // Validate message
                if (header.magic != MESSAGE_MAGIC) {
                    if (corruption_count.load(std::memory_order_relaxed) < 5) {
                        worklet_debug("ERROR: Invalid magic at tail=%d head=%d: got 0x%08X expected 0x%08X (len=%u seq=%u)",
                                     in_tail, in_head, header.magic, MESSAGE_MAGIC, header.length, header.sequence);
                        corruption_count.fetch_add(1, std::memory_order_relaxed);
                    }
                    control->in_tail.store((in_tail + 1) % IN_BUFFER_SIZE, std::memory_order_release);
                    metrics->messages_dropped.fetch_add(1, std::memory_order_relaxed);
                    in_tail = control->in_tail.load(std::memory_order_acquire);
                    continue;
                }

                if (header.length > MAX_MESSAGE_SIZE + sizeof(Message)) {
                    control->status_flags.fetch_or(STATUS_FRAGMENTED_MSG, std::memory_order_relaxed);
                    control->in_tail.store((in_tail + header.length) % IN_BUFFER_SIZE, std::memory_order_release);
                    metrics->messages_dropped.fetch_add(1, std::memory_order_relaxed);
                    in_tail = control->in_tail.load(std::memory_order_acquire);
                    continue;
                }

                // Extract OSC payload (skip message header)
                uint32_t payload_size = header.length - sizeof(Message);

                // Validate payload size
                if (payload_size > MAX_MESSAGE_SIZE) {
                    control->in_tail.store((in_tail + header.length) % IN_BUFFER_SIZE, std::memory_order_release);
                    metrics->messages_dropped.fetch_add(1, std::memory_order_relaxed);
                    in_tail = control->in_tail.load(std::memory_order_acquire);
                    continue;
                }

                // Gap detection: check for missing messages
                int32_t prev_seq = last_in_sequence.load(std::memory_order_relaxed);
                if (prev_seq >= 0) {
                    int32_t expected = (prev_seq + 1) & 0x7FFFFFFF;  // Handle wrap at INT32_MAX
                    if ((int32_t)header.sequence != expected) {
                        // Gap detected - messages were lost
                        int32_t gap_size = ((int32_t)header.sequence - expected + 0x80000000) & 0x7FFFFFFF;
                        if (gap_size > 0 && gap_size < 1000) {  // Sanity check - ignore huge gaps (likely reset)
                            metrics->messages_sequence_gaps.fetch_add(gap_size, std::memory_order_relaxed);
                            if (gap_log_count.load(std::memory_order_relaxed) < 5) {
                                worklet_debug("WARNING: Sequence gap detected: expected %d, got %u (gap of %d)",
                                             expected, header.sequence, gap_size);
                                gap_log_count.fetch_add(1, std::memory_order_relaxed);
                            }
                        }
                    }
                }
                last_in_sequence.store((int32_t)header.sequence, std::memory_order_relaxed);

                // Use static buffer - local 768KB buffer would overflow WASM stack!
                char* osc_buffer = static_osc_buffer;

                // Copy OSC payload from ring buffer (may be split across wrap boundary)
                uint32_t payload_start = (in_tail + sizeof(Message)) % IN_BUFFER_SIZE;
                uint32_t payload_offset = IN_BUFFER_START + payload_start;
                uint32_t bytes_to_end = IN_BUFFER_SIZE - payload_start;

                if (payload_size <= bytes_to_end) {
                    // Payload fits contiguously
                    std::memcpy(osc_buffer, shared_memory + payload_offset, payload_size);
                } else {
                    // Payload is split - read in two parts
                    std::memcpy(osc_buffer, shared_memory + payload_offset, bytes_to_end);
                    std::memcpy(osc_buffer + bytes_to_end, shared_memory + IN_BUFFER_START, payload_size - bytes_to_end);
                }

                // RT-SAFE message processing - no malloc!
                // Setup reply address - zero-initialize for consistent comparison in /notify
                ReplyAddress reply_addr = {};
                reply_addr.mProtocol = kWeb;
                reply_addr.mReplyFunc = osc_reply_to_ring_buffer;
                reply_addr.mReplyData = nullptr;

                bool is_bundle_msg = is_bundle(osc_buffer, payload_size);

                if (is_bundle_msg) {
                    // Extract timetag
                    uint64_t timetag = extract_timetag(osc_buffer);

                    // Check if immediate execution (timetag == 0 or 1)
                    if (timetag == 0 || timetag == 1) {
                        // Immediate bundle - execute now
                        OSC_Packet packet;
                        packet.mData = osc_buffer;
                        packet.mSize = payload_size;
                        packet.mIsBundle = true;
                        packet.mReplyAddr = reply_addr;

                        PerformOSCBundle(g_world, &packet);
                    } else {
                        // Future bundle - check if scheduler has room first (backpressure)
                        if (g_scheduler.IsFull()) {
                            // Scheduler full - leave message in ring buffer for next callback
                            // Reset sequence tracking so next iteration processes this message correctly
                            last_in_sequence.store((header.sequence > 0) ? (int32_t)(header.sequence - 1) : -1, std::memory_order_relaxed);
                            worklet_debug("INFO: Scheduler full (%d events), backpressure - message stays in ring buffer",
                                         g_scheduler.Size());
                            break;  // Exit message processing loop
                        }

                        // Future bundle - schedule it (RT-safe, no malloc!)
                        int64_t current_osc_time = ntp_to_osc_timetag(current_ntp);

                        if (!schedule_bundle(g_world, (int64_t)timetag, current_osc_time, osc_buffer, payload_size, reply_addr)) {
                            // This shouldn't happen now since we check IsFull() first
                            worklet_debug("ERROR: Failed to schedule bundle (unexpected)");
                        }
                    }
                } else {
                    // Single OSC message - execute immediately
                    PerformOSCMessage(g_world, payload_size, osc_buffer, &reply_addr);
                }

                // Update IN tail (consume message)
                control->in_tail.store((in_tail + header.length) % IN_BUFFER_SIZE, std::memory_order_release);
                metrics->messages_processed.fetch_add(1, std::memory_order_relaxed);
                messages_this_frame++;

                // Update tail for next iteration
                in_tail = control->in_tail.load(std::memory_order_acquire);
            }

            // Block size from scsynth's World options. Web: always 128
            // (AudioWorklet render quantum). Native: chosen at boot —
            // typically equal to the hardware callback buffer size.
            const int QUANTUM_SIZE = g_world->mBufLength;

            // Zero OUTPUT audio buses for this render cycle
            uint32_t output_bus_bytes = g_world->mNumOutputs * g_world->mBufLength * sizeof(float);
            memset(g_world->mAudioBus, 0, output_bus_bytes);

            // CRITICAL: Also zero static_audio_bus to prevent accumulation across frames
            memset(static_audio_bus, 0, QUANTUM_SIZE * g_world->mNumOutputs * sizeof(float));

            // Increment buffer counter once per audio frame
            g_world->mBufCounter++;

            // EXECUTE SCHEDULED BUNDLES (from SC_CoreAudio.cpp:1388-1401)
            int64_t currentOscTime = ntp_to_osc_timetag(current_ntp);
            int64_t nextOscTime = currentOscTime + g_osc_increment;

            // Execute all bundles that are due within this buffer
            int64_t schedTime;
            while ((schedTime = g_scheduler.NextTime()) <= nextOscTime) {
                // Calculate sub-sample offset (from SC_CoreAudio.cpp:1389-1397)
                float diffTime = (float)(schedTime - currentOscTime) * g_osc_to_samples + 0.5;
                float diffTimeFloor = floor(diffTime);
                g_world->mSampleOffset = (int)diffTimeFloor;
                g_world->mSubsampleOffset = diffTime - diffTimeFloor;

                // Clamp to buffer bounds [0, bufLen-1]
                if (g_world->mSampleOffset < 0)
                    g_world->mSampleOffset = 0;
                else if (g_world->mSampleOffset >= g_world->mBufLength)
                    g_world->mSampleOffset = g_world->mBufLength - 1;

                // Get pointer to bundle in pool (no 1KB copy!)
                ScheduledBundle* bundle = g_scheduler.Remove();
                update_scheduler_depth_metric(g_scheduler.Size());

                if (!bundle) {
                    break;  // Should not happen, but be safe
                }

                // Late bundle detection - track in metrics and warn when timing is broken
                int64_t time_diff_osc = schedTime - currentOscTime;
                double time_diff_ms = ((double)time_diff_osc / 4294967296.0) * 1000.0;

                // Bundles within the current quantum are not late — they arrive when
                // the VM's sleep target doesn't align with quantum boundaries.
                // scsynth processes them at the correct sub-sample offset (lines above).
                // Only bundles older than one quantum are genuinely late.
                double quantum_ms = (1000.0 * QUANTUM_SIZE) / g_world->mSampleRate;
                if (time_diff_ms < -quantum_ms) {
                    // Cap late_ms to prevent overflow from timing sync issues
                    // Values over 10 seconds indicate a systemic problem, not individual lateness
                    double raw_late_ms = -time_diff_ms;
                    int32_t late_ms = (raw_late_ms > 10000.0) ? 10000 : (int32_t)raw_late_ms;
                    int late_now = late_count.fetch_add(1, std::memory_order_relaxed) + 1;
                    metrics->scheduler_lates.fetch_add(1, std::memory_order_relaxed);

                    // Track max lateness (compare-exchange loop for atomic max)
                    int32_t current_max = metrics->scheduler_max_late_ms.load(std::memory_order_relaxed);
                    while (late_ms > current_max) {
                        if (metrics->scheduler_max_late_ms.compare_exchange_weak(
                                current_max, late_ms, std::memory_order_relaxed, std::memory_order_relaxed)) {
                            break;
                        }
                        // current_max is updated by compare_exchange_weak on failure
                    }

                    // Store last late magnitude and tick for correlation
                    metrics->scheduler_last_late_ms.store(late_ms, std::memory_order_relaxed);
                    metrics->scheduler_last_late_tick.store(
                        metrics->process_count.load(std::memory_order_relaxed),
                        std::memory_order_relaxed);

                    if (late_now == 1 || late_now % 100 == 0) {
                        // Extract OSC address from first message in bundle
                        const char* addr = "?";
                        const uint8_t* bdata = g_scheduler.DataPool() + bundle->mDataOffset;
                        if (bundle->mSize > 20) {
                            addr = reinterpret_cast<const char*>(bdata + 20);
                        }
                        worklet_debug("LATE: %.1fms %s (count=%d)", -time_diff_ms, addr, late_now);
                    }
                }

                bundle->Perform(g_scheduler.DataPool()); // Execute from data pool
                g_scheduler.ReleaseSlot(bundle);          // Return slot + maybe reset pool
            }

            // Reset offset
            g_world->mSampleOffset = 0;
            g_world->mSubsampleOffset = 0.0;

            // Mark input buses as "touched" so In.ar UGen reads them
            // The JS worklet copies audio to input bus area before calling process_audio()
            // Without this, In.ar will output silence (it checks mAudioBusTouched)
            if (active_input_channels > 0) {
                uint32_t active_in = std::min(active_input_channels, static_cast<uint32_t>(g_world->mNumInputs));
                // Input buses start after output buses in mAudioBusTouched
                int32_t* inputTouched = g_world->mAudioBusTouched + g_world->mNumOutputs;
                const int32_t bufCounter = g_world->mBufCounter;
                for (uint32_t i = 0; i < active_in; i++) {
                    inputTouched[i] = bufCounter;
                }
            }

            // Run scsynth to generate audio (128 samples). Guarded so the
            // test binary's new/delete hooks count any allocation in the
            // DSP pass — UGen Calc functions must not allocate. Scoped
            // tightly to World_Run; synthdef parsing (/d_recv), synth
            // construction (/s_new), and reply formatting (FIFO Performs
            // below) are not covered. Dedicated [rt_alloc] tests exercise
            // those paths explicitly.
            {
                rt_alloc::Guard rt_dsp_guard;
                World_Run(g_world);
            }

            // Process notification FIFOs to send /tr, /n_end, /n_go, etc. messages
            g_world->hw->mTriggers.Perform();
            g_world->hw->mNodeMsgs.Perform();
            g_world->hw->mNodeEnds.Perform();

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
#endif
        }

        return true; // Keep processor alive
    }

    // Core implementation - write formatted message to ring buffer
    static int worklet_debug_impl(const char* fmt, va_list args) {
        if (!memory_initialized) return 0;

        // Format the message
        char buffer[1024];
        int result = vsnprintf(buffer, sizeof(buffer), fmt, args);

        // Calculate message length (including newline)
        uint32_t msg_len = 0;
        while (buffer[msg_len] != '\0' && msg_len < sizeof(buffer)) {
            msg_len++;
        }

        // Add newline to buffer
        if (msg_len < sizeof(buffer) - 1) {
            buffer[msg_len] = '\n';
            msg_len++;
        }

        // Use unified ring buffer write with full protection (:: for global scope)
        ::ring_buffer_write(
            shared_memory,              // buffer_start
            DEBUG_BUFFER_SIZE,          // buffer_size
            DEBUG_BUFFER_START,         // buffer_start_offset
            &control->debug_head,       // head
            &control->debug_tail,       // tail
            buffer,                     // data
            msg_len,                    // data_size
            nullptr                     // metrics (not tracked for debug currently)
        );

        return result;
    }

    // Variadic version - for direct C++ calls
    extern "C" EMSCRIPTEN_KEEPALIVE
    int worklet_debug(const char* fmt, ...) {
        va_list args;
        va_start(args, fmt);
        int result = worklet_debug_impl(fmt, args);
        va_end(args);
        return result;
    }

    // va_list version - for function pointers (matches PrintFunc signature)
    extern "C" EMSCRIPTEN_KEEPALIVE
    int worklet_debug_va(const char* fmt, va_list args) {
        return worklet_debug_impl(fmt, args);
    }

    // Raw version - writes pre-formatted message directly to ring buffer
    // Use this when you already have a formatted string (avoids double-copy)
    extern "C" EMSCRIPTEN_KEEPALIVE
    int worklet_debug_raw(const char* msg, uint32_t len) {
        if (!memory_initialized || !msg || len == 0) return 0;

        // Use unified ring buffer write with full protection (:: for global scope)
        ::ring_buffer_write(
            shared_memory,              // buffer_start
            DEBUG_BUFFER_SIZE,          // buffer_size
            DEBUG_BUFFER_START,         // buffer_start_offset
            &control->debug_head,       // head
            &control->debug_tail,       // tail
            msg,                        // data
            len,                        // data_size
            nullptr                     // metrics (not tracked for debug currently)
        );

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

    // scsynth audio output accessors
    // Returns the accumulated audio buffer (128 samples per channel)
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
        // was configured when the World was built.
        return g_world ? g_world->mBufLength : sonicpi::kDefaultBlockSize;
    }

    // scsynth audio input accessor
    // Returns pointer to input bus area in mAudioBus (after output buses)
    // Layout: mAudioBus = [output buses][input buses][internal buses]
    EMSCRIPTEN_KEEPALIVE
    uintptr_t get_audio_input_bus() {
        if (!memory_initialized || !g_world) {
            return 0;
        }

        // Input buses start after output buses in mAudioBus
        // Each bus has mBufLength samples (128)
        return reinterpret_cast<uintptr_t>(
            g_world->mAudioBus + (g_world->mNumOutputs * g_world->mBufLength)
        );
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

/**
 * Unified ring buffer write function with full corruption protection.
 *
 * This function implements a lock-free SPSC (Single Producer Single Consumer) ring buffer
 * with the following guarantees:
 * - Messages are always written contiguously (no wrapping mid-message)
 * - Automatic padding insertion when wrapping is needed
 * - Buffer overflow detection with graceful message dropping
 * - Atomic memory operations for thread safety
 * - Metrics tracking for dropped messages and overruns
 *
 * @param buffer_start Physical memory address of the buffer
 * @param buffer_size Total size of the ring buffer in bytes
 * @param buffer_start_offset Offset within shared memory where buffer starts
 * @param head Atomic pointer to head position (producer writes here)
 * @param tail Atomic pointer to tail position (consumer reads from here)
 * @param data Payload data to write
 * @param data_size Size of payload in bytes
 * @param metrics Optional metrics structure for tracking drops/overruns
 * @return true if message written successfully, false if dropped due to insufficient space
 */
bool ring_buffer_write(
    uint8_t* buffer_start,
    uint32_t buffer_size,
    uint32_t buffer_start_offset,
    std::atomic<int32_t>* head,
    std::atomic<int32_t>* tail,
    const void* data,
    uint32_t data_size,
    PerformanceMetrics* metrics
) {
    using namespace scsynth;  // Access globals from scsynth namespace

    // Create message header
    Message header;
    header.magic = MESSAGE_MAGIC;
    header.length = sizeof(Message) + data_size;

    // Use appropriate sequence counter based on which buffer we're writing to
    // This prevents false "dropped message" detection when debug and OSC messages interleave
    if (buffer_start_offset == OUT_BUFFER_START) {
        header.sequence = control->out_sequence.fetch_add(1, std::memory_order_relaxed);
    } else {
        // DEBUG_BUFFER_START
        header.sequence = control->debug_sequence.fetch_add(1, std::memory_order_relaxed);
    }

    // Load head and tail with acquire semantics
    int32_t current_head = head->load(std::memory_order_acquire);
    int32_t current_tail = tail->load(std::memory_order_acquire);

    // Calculate available space in the buffer
    uint32_t available = (buffer_size - 1 - current_head + current_tail) % buffer_size;

    // Check if there's enough space for the message
    if (available < header.length) {
        // Not enough space - drop the message and track metrics
        if (metrics) {
            metrics->messages_dropped.fetch_add(1, std::memory_order_relaxed);
        }
        if (control) {
            control->status_flags.fetch_or(STATUS_BUFFER_FULL, std::memory_order_relaxed);
        }
        return false;
    }

    // Check if message fits contiguously, otherwise write padding and wrap to 0.
    //
    // Design note: OUT/DEBUG buffers use contiguous-only writes (with padding markers)
    // rather than split writes that wrap around the boundary. This keeps the C++ writer
    // simple (one memcpy per message on the audio thread) and the JS reader simple
    // (contiguous reads, no wrap-around logic for payloads). The IN buffer uses a
    // different design (split writes via RingBufferWriter.h) because it's written from
    // JS where the complexity tradeoff is different.
    //
    // After wrapping, we must re-check that there's enough space between position 0
    // and tail — the initial available-space check included bytes that will be wasted
    // as padding, so it can overestimate the usable space at the front.
    uint32_t space_to_end = buffer_size - current_head;
    if (header.length > space_to_end) {
        // Verify space at front after wrap (tail-1 to avoid head==tail ambiguity)
        uint32_t space_at_front = (current_tail > 0) ? (current_tail - 1) : 0;
        if (space_at_front < header.length) {
            if (metrics) {
                metrics->messages_dropped.fetch_add(1, std::memory_order_relaxed);
            }
            if (control) {
                control->status_flags.fetch_or(STATUS_BUFFER_FULL, std::memory_order_relaxed);
            }
            return false;
        }

        if (space_to_end >= sizeof(Message)) {
            // Write padding marker to fill remaining space
            Message padding;
            padding.magic = PADDING_MAGIC;
            padding.length = 0;
            padding.sequence = 0;

            std::memcpy(buffer_start + buffer_start_offset + current_head, &padding, sizeof(Message));
        } else if (space_to_end >= 4) {
            // Not enough room for full padding header but enough for the magic word.
            // Write PADDING_MAGIC so the reader recognises this as a wrap marker
            // (without this, zeroed bytes look like corruption to the reader).
            uint32_t pad = PADDING_MAGIC;
            std::memcpy(buffer_start + buffer_start_offset + current_head, &pad, 4);
            if (space_to_end > 4) {
                std::memset(buffer_start + buffer_start_offset + current_head + 4, 0, space_to_end - 4);
            }
        }

        // Wrap head to beginning
        current_head = 0;
    }

    // Write message header (now contiguous)
    std::memcpy(buffer_start + buffer_start_offset + current_head, &header, sizeof(Message));

    // Write payload (contiguous)
    std::memcpy(buffer_start + buffer_start_offset + current_head + sizeof(Message), data, data_size);

    // Update head pointer with release semantics (publish message)
    int32_t new_head = (current_head + header.length) % buffer_size;
    head->store(new_head, std::memory_order_release);

    // Track peak buffer usage at write time — the reader may drain the
    // buffer before the periodic metrics sampling sees the fill level.
    if (metrics && buffer_start_offset == OUT_BUFFER_START) {
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
void osc_reply_to_ring_buffer(ReplyAddress* addr, char* msg, int size) {
    using namespace scsynth;  // Access globals from scsynth namespace

    if (!control || !shared_memory) {
        return;
    }

    // Use unified ring buffer write with full protection
    ring_buffer_write(
        shared_memory,              // buffer_start
        OUT_BUFFER_SIZE,            // buffer_size
        OUT_BUFFER_START,           // buffer_start_offset
        &control->out_head,         // head
        &control->out_tail,         // tail
        msg,                        // data
        size,                       // data_size
        metrics                     // metrics
    );
}
