/*
    SuperSonic - SuperCollider AudioWorklet WebAssembly port
    Copyright (c) 2025 Sam Aaron

    Based on SuperCollider by James McCartney and community
    GPL v3 or later
*/

#include "audio_processor.h"
#include "build_info.h"  // Build hash and timestamp
#include <emscripten/webaudio.h>
#include <algorithm>
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
extern "C" {
    static int global_errno = 0;

    int* __errno_location() {
        return &global_errno;
    }
}

// Include SuperCollider version info
#include "scsynth/common/SC_Version.hpp"

// Supersonic version - appended to SC version
static const int SUPERSONIC_VERSION_MAJOR = 0;
static const int SUPERSONIC_VERSION_MINOR = 10;
static const int SUPERSONIC_VERSION_PATCH = 0;

// Global pointers
extern "C" {
    // Static ring buffer allocated in WASM data segment
    // This ensures no conflicts with scsynth heap allocations
    // IMPORTANT: Must be 8-byte aligned for Float64Array access from JavaScript
    // Size: ~1.4MB (IN: 768KB, OUT: 128KB, DEBUG: 64KB, control/metrics, node tree ~57KB, audio capture ~375KB)
    alignas(8) uint8_t ring_buffer_storage[TOTAL_BUFFER_SIZE];

    // Validate at compile time that buffer layout fits in allocated storage
    static_assert(TOTAL_BUFFER_SIZE <= sizeof(ring_buffer_storage),
                  "Buffer layout exceeds allocated storage!");

    // Static audio bus buffer (128 channels * 128 samples * 4 bytes = 64KB)
    // Pre-allocated to avoid malloc in critical audio path
    alignas(16) float static_audio_bus[128 * 128];

    uint8_t* shared_memory = nullptr;
    ControlPointers* control = nullptr;
    PerformanceMetrics* metrics = nullptr;
    double* ntp_start_time = nullptr;        // NEW
    std::atomic<int32_t>* drift_offset = nullptr;  // NEW
    std::atomic<int32_t>* global_offset = nullptr; // NEW
    AudioCaptureHeader* audio_capture = nullptr;   // Audio capture for testing
    float* audio_capture_data = nullptr;           // Audio capture data buffer
    bool memory_initialized = false;
    World* g_world = nullptr;

    // OSC Bundle Scheduler - Index-based pool for RT-safety
    // Events stored in pool (never copied), queue only stores small indices
    BundleScheduler g_scheduler;

    // Time conversion constants - Based on SC_CoreAudio.cpp
    const uint64_t SECONDS_1900_TO_1970 = 2208988800ULL;
    double g_osc_increment_numerator = 0.0;  // Buffer length in NTP units
    int64_t g_osc_increment = 0;             // NTP units per buffer
    double g_osc_to_samples = 0.0;           // NTP units -> samples conversion
    double g_time_zero_osc = 0.0;            // AudioContext time -> OSC time offset
    bool g_time_initialized = false;         // Have we set up time conversion?

    // Return the base address of the ring buffer
    // JavaScript will use this to calculate all buffer positions
    EMSCRIPTEN_KEEPALIVE
    int get_ring_buffer_base() {
        return reinterpret_cast<int>(ring_buffer_storage);
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
        worklet_debug("Time offset set from JavaScript: %.6f", offset);
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

    // Convert AudioContext time (double) to OSC/NTP time (int64)
    int64_t audio_to_osc_time(double audio_time) {
        double osc_seconds = audio_time + g_time_zero_osc;
        uint32_t seconds = (uint32_t)osc_seconds;
        uint32_t fraction = (uint32_t)((osc_seconds - seconds) * 4294967296.0);
        // Use unsigned arithmetic to avoid sign extension, then cast to signed
        uint64_t result = ((uint64_t)seconds << 32) | fraction;
        return (int64_t)result;
    }

    // RT-safe bundle scheduling - no malloc!
    // Returns true if scheduled, false if queue full
    bool schedule_bundle(World* world, int64_t ntp_time, int64_t current_osc_time,
                        const char* data, int32_t size, const ReplyAddress& reply_addr) {
        if (size > 8192) {
            worklet_debug("ERROR: Bundle too large: %d bytes", size);
            return false;
        }

        // Add directly to scheduler pool (no 8KB copy - data copied into pool slot)
        if (!g_scheduler.Add(world, ntp_time, data, size, reply_addr)) {
            worklet_debug("ERROR: Scheduler queue full (%d events)", g_scheduler.Size());
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
        metrics = reinterpret_cast<PerformanceMetrics*>(shared_memory + METRICS_START);

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

        // Initialize node tree memory
        // All entries start with id = -1 (empty slot)
        // Using memset with 0xFF sets all bytes to 0xFF, which is -1 for signed int32
        uint8_t* node_tree_ptr = shared_memory + NODE_TREE_START;
        memset(node_tree_ptr, 0xFF, NODE_TREE_SIZE);

        // Initialize header properly (count=0, version=0)
        NodeTreeHeader* tree_header = reinterpret_cast<NodeTreeHeader*>(node_tree_ptr);
        tree_header->node_count.store(0, std::memory_order_relaxed);
        tree_header->version.store(0, std::memory_order_relaxed);

        worklet_debug("[NodeTree] Initialized at offset %u, size %u bytes",
                     NODE_TREE_START, NODE_TREE_SIZE);

        // Initialize audio capture
        audio_capture = reinterpret_cast<AudioCaptureHeader*>(shared_memory + AUDIO_CAPTURE_START);
        audio_capture_data = reinterpret_cast<float*>(shared_memory + AUDIO_CAPTURE_START + AUDIO_CAPTURE_HEADER_SIZE);
        audio_capture->enabled.store(0, std::memory_order_relaxed);  // Disabled by default
        audio_capture->head.store(0, std::memory_order_relaxed);
        audio_capture->sample_rate = static_cast<uint32_t>(sample_rate);
        audio_capture->channels = AUDIO_CAPTURE_CHANNELS;

        // Enable worklet_debug
        memory_initialized = true;

        // Boot message shown after ASCII art below

        // Read worldOptions from SharedArrayBuffer (written by JS)
        // WorldOptions location: ringBufferBase + 65536 (after ring_buffer_storage)
        uint32_t* worldOptionsPtr = (uint32_t*)((uint8_t*)ring_buffer_storage + 65536);

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
        options.mBufLength = worldOptionsPtr[8];                    // From JS (must be 128)
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

        worklet_debug("░█▀▀░█░█░█▀█░█▀▀░█▀▄░█▀▀░█▀█░█▀█░▀█▀░█▀▀");
        worklet_debug("░▀▀█░█░█░█▀▀░█▀▀░█▀▄░▀▀█░█░█░█░█░░█░░█░░");
        worklet_debug("░▀▀▀░▀▀▀░▀░░░▀▀▀░▀░▀░▀▀▀░▀▀▀░▀░▀░▀▀▀░▀▀▀");
        worklet_debug("v%d.%d.%d (scsynth %d.%d.%d) %.0fkHz %dch",
                     SUPERSONIC_VERSION_MAJOR, SUPERSONIC_VERSION_MINOR, SUPERSONIC_VERSION_PATCH,
                     SC_VersionMajor, SC_VersionMinor, SC_VersionPatch,
                     sample_rate / 1000, options.mNumOutputBusChannels);
    }

    // Main audio processing function - called every audio frame (128 samples)
    // current_time: AudioContext.currentTime (for reference, may not be used)
    // current_ntp: Current NTP time from performance.now() (drift-free)
    EMSCRIPTEN_KEEPALIVE
    bool process_audio(double current_time) {
        if (!memory_initialized) {
            return true; // Keep alive but do nothing if not initialized
        }

        if (!metrics) {
            return false;
        }

        // Calculate current NTP time from components
        // currentNTP = audioContextTime + ntp_start + (drift_ms/1000) + (global_ms/1000)
        // Cache ntp_start_time: keep trying until non-zero, then refresh every 15s
        static double cached_ntp_start = 0.0;
        static uint32_t last_cache_update = 0;
        uint32_t current_frame = metrics->process_count.load(std::memory_order_relaxed);

        // Update cache: every frame if still 0.0, else every 15s (720000 frames at 48kHz)
        bool should_update = (cached_ntp_start == 0.0) ||
                            (current_frame - last_cache_update >= 720000);
        if (should_update) {
            cached_ntp_start = (ntp_start_time && *ntp_start_time != 0.0) ? *ntp_start_time : 0.0;
            last_cache_update = current_frame;
        }

        double ntp_start = cached_ntp_start;
        double drift_seconds = drift_offset ? (drift_offset->load(std::memory_order_relaxed) / 1000.0) : 0.0;
        double global_seconds = global_offset ? (global_offset->load(std::memory_order_relaxed) / 1000.0) : 0.0;

        double current_ntp = current_time + ntp_start + drift_seconds + global_seconds;

        metrics->process_count.fetch_add(1, std::memory_order_relaxed);

        // Process incoming OSC messages if scsynth is ready
        if (g_world) {
            int32_t in_head = control->in_head.load(std::memory_order_acquire);
            int32_t in_tail = control->in_tail.load(std::memory_order_acquire);


            // Process all available messages
            while (in_head != in_tail) {
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
                    // Log first few corruption events for diagnostics
                    static uint32_t corruption_count = 0;
                    if (corruption_count < 5) {
                        worklet_debug("ERROR: Invalid magic at tail=%d head=%d: got 0x%08X expected 0x%08X (len=%u seq=%u)",
                                     in_tail, in_head, header.magic, MESSAGE_MAGIC, header.length, header.sequence);
                        corruption_count++;
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
                // Uses static to persist across process() calls
                static int32_t last_in_sequence = -1;
                if (last_in_sequence >= 0) {
                    int32_t expected = (last_in_sequence + 1) & 0x7FFFFFFF;  // Handle wrap at INT32_MAX
                    if ((int32_t)header.sequence != expected) {
                        // Gap detected - messages were lost
                        int32_t gap_size = ((int32_t)header.sequence - expected + 0x80000000) & 0x7FFFFFFF;
                        if (gap_size > 0 && gap_size < 1000) {  // Sanity check - ignore huge gaps (likely reset)
                            metrics->messages_sequence_gaps.fetch_add(gap_size, std::memory_order_relaxed);
                            static uint32_t gap_log_count = 0;
                            if (gap_log_count < 5) {
                                worklet_debug("WARNING: Sequence gap detected: expected %d, got %u (gap of %d)",
                                             expected, header.sequence, gap_size);
                                gap_log_count++;
                            }
                        }
                    }
                }
                last_in_sequence = (int32_t)header.sequence;

                char osc_buffer[MAX_MESSAGE_SIZE];

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
                // Setup reply address
                ReplyAddress reply_addr;
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
                            last_in_sequence = (header.sequence > 0) ? (int32_t)(header.sequence - 1) : -1;
                            worklet_debug("INFO: Scheduler full (%d events), backpressure - message stays in ring buffer",
                                         g_scheduler.Size());
                            break;  // Exit message processing loop
                        }

                        // Future bundle - schedule it (RT-safe, no malloc!)
                        // Convert current NTP time to OSC timetag format (int64)
                        uint32_t seconds = (uint32_t)current_ntp;
                        uint32_t fraction = (uint32_t)((current_ntp - seconds) * 4294967296.0);
                        uint64_t current_osc_time_u = ((uint64_t)seconds << 32) | fraction;
                        int64_t current_osc_time = (int64_t)current_osc_time_u;

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

                // Update tail for next iteration
                in_tail = control->in_tail.load(std::memory_order_acquire);
            }

            // AudioWorklet provides 128 samples per quantum, and we configure SC to match
            const int QUANTUM_SIZE = 128;

            // Zero audio buses for this render cycle
            // SuperCollider expects buses to start at 0.0f each frame
            uint32_t bus_size_bytes = g_world->mNumAudioBusChannels * g_world->mBufLength * sizeof(float);
            memset(g_world->mAudioBus, 0, bus_size_bytes);

            // CRITICAL: Also zero static_audio_bus to prevent accumulation across frames
            memset(static_audio_bus, 0, QUANTUM_SIZE * g_world->mNumOutputs * sizeof(float));

            // Increment buffer counter once per audio frame
            g_world->mBufCounter++;

            // EXECUTE SCHEDULED BUNDLES (from SC_CoreAudio.cpp:1388-1401)
            // Convert current NTP time to OSC timetag format
            uint32_t seconds = (uint32_t)current_ntp;
            uint32_t fraction = (uint32_t)((current_ntp - seconds) * 4294967296.0);
            uint64_t currentOscTime_u = ((uint64_t)seconds << 32) | fraction;
            int64_t currentOscTime = (int64_t)currentOscTime_u;
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

                // Get pointer to bundle in pool (no 8KB copy!)
                ScheduledBundle* bundle = g_scheduler.Remove();
                update_scheduler_depth_metric(g_scheduler.Size());

                if (!bundle) {
                    break;  // Should not happen, but be safe
                }

                // Late bundle detection - warn when timing is broken
                int64_t time_diff_osc = schedTime - currentOscTime;
                double time_diff_ms = ((double)time_diff_osc / 4294967296.0) * 1000.0;

                // Warn if bundle is late (>3ms) or negative (indicates broken timing)
                if (time_diff_ms > 3.0 || time_diff_ms < 0.0) {
                    worklet_debug("LATE: Bundle executing - timetag=%llu current=%llu diff=%.2fms offset=%d subsample=%.3f",
                                 (unsigned long long)schedTime, (unsigned long long)currentOscTime,
                                 time_diff_ms, g_world->mSampleOffset, g_world->mSubsampleOffset);
                }

                bundle->Perform();  // Execute the bundle
                bundle->Release();  // Return slot to pool
            }

            // Reset offset
            g_world->mSampleOffset = 0;
            g_world->mSubsampleOffset = 0.0;

            // Run scsynth to generate audio (128 samples)
            World_Run(g_world);

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
            // Each channel has 128 samples, we have 2 channels = 256 total floats
            // 256 / 4 = 64 SIMD operations
            const int total_samples = g_world->mNumOutputs * QUANTUM_SIZE;
            const int simd_iterations = total_samples / 4;

            for (int i = 0; i < simd_iterations; i++) {
                v128_t vec = wasm_v128_load(src + i * 4);
                wasm_v128_store(dst + i * 4, vec);
            }

            // Handle any remaining samples (shouldn't be any with 256 samples)
            const int remaining = total_samples % 4;
            if (remaining > 0) {
                memcpy(dst + simd_iterations * 4, src + simd_iterations * 4, remaining * sizeof(float));
            }
#else
            // Fallback to memcpy if SIMD not available
            memcpy(dst, src, g_world->mNumOutputs * QUANTUM_SIZE * sizeof(float));
#endif

            // Audio capture for testing - copy interleaved audio to capture buffer
            if (audio_capture && audio_capture->enabled.load(std::memory_order_relaxed)) {
                uint32_t head = audio_capture->head.load(std::memory_order_relaxed);
                const uint32_t channels = g_world->mNumOutputs;
                const uint32_t frames_to_copy = QUANTUM_SIZE;

                // Check if we have room in the capture buffer
                if (head + frames_to_copy <= AUDIO_CAPTURE_FRAMES) {
                    // Copy audio data - interleave channels for easier JS processing
                    // Source is channel-by-channel (ch0[0..127], ch1[0..127])
                    // Destination is interleaved (ch0[0], ch1[0], ch0[1], ch1[1], ...)
                    for (uint32_t frame = 0; frame < frames_to_copy; frame++) {
                        for (uint32_t ch = 0; ch < channels && ch < AUDIO_CAPTURE_CHANNELS; ch++) {
                            audio_capture_data[(head + frame) * AUDIO_CAPTURE_CHANNELS + ch] =
                                static_audio_bus[ch * QUANTUM_SIZE + frame];
                        }
                    }
                    audio_capture->head.store(head + frames_to_copy, std::memory_order_release);
                } else {
                    // Buffer full - log once and stop capturing
                    static bool logged_buffer_full = false;
                    if (!logged_buffer_full) {
                        worklet_debug("[AudioCapture] Buffer full (%u frames), capture stopped", AUDIO_CAPTURE_FRAMES);
                        logged_buffer_full = true;
                    }
                }
            }
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
    // Returns the accumulated audio buffer (128 samples per channel from double-loop)
    EMSCRIPTEN_KEEPALIVE
    uintptr_t get_audio_output_bus() {
        if (!memory_initialized) {
            return 0;
        }

        return reinterpret_cast<uintptr_t>(static_audio_bus);
    }

    EMSCRIPTEN_KEEPALIVE
    int get_audio_buffer_samples() {
        return 128; // AudioWorklet quantum size
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

    // Test function to check wave table values
    EMSCRIPTEN_KEEPALIVE
    float get_sine_wavetable_value(int index) {
        if (index < 0 || index >= 2 * 8192) {
            return -999.0f;  // Error value
        }
        return gSineWavetable[index];
    }

} // namespace scsynth

// ============================================================================
// RING BUFFER HELPER FUNCTIONS (outside namespace for C++ linkage)
// ============================================================================

// Ring buffer helper: get next index with wrap
inline uint32_t next_index(uint32_t idx, uint32_t buffer_size) {
    return (idx + 1) % buffer_size;
}

// Ring buffer helper: calculate available space
inline uint32_t available_space(uint32_t head, uint32_t tail, uint32_t buffer_size) {
    return (buffer_size - 1 - head + tail) % buffer_size;
}

// Ring buffer helper: check if buffer is full
inline bool is_buffer_full(uint32_t head, uint32_t tail, uint32_t buffer_size) {
    return next_index(head, buffer_size) == tail;
}

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

    // Check if message fits contiguously, otherwise write padding (if possible) and wrap
    uint32_t space_to_end = buffer_size - current_head;
    if (header.length > space_to_end) {
        if (space_to_end >= sizeof(Message)) {
            // Write padding marker to fill remaining space
            Message padding;
            padding.magic = PADDING_MAGIC;
            padding.length = 0;
            padding.sequence = 0;

            std::memcpy(buffer_start + buffer_start_offset + current_head, &padding, sizeof(Message));
        } else if (space_to_end > 0) {
            // Not enough room for padding header – clear remaining bytes
            std::memset(buffer_start + buffer_start_offset + current_head, 0, space_to_end);
        }

        // Wrap head to beginning
        current_head = 0;
    }

    // Write message header (now contiguous)
    std::memcpy(buffer_start + buffer_start_offset + current_head, &header, sizeof(Message));

    // Write payload (contiguous)
    std::memcpy(buffer_start + buffer_start_offset + current_head + sizeof(Message), data, data_size);

    // Update head pointer with release semantics (publish message)
    head->store((current_head + header.length) % buffer_size, std::memory_order_release);

    return true;
}

// OSC reply callback for scsynth
// This is called by scsynth when it needs to send OSC replies (e.g., /done, /n_go, etc.)
void osc_reply_to_ring_buffer(ReplyAddress* addr, char* msg, int size) {
    using namespace scsynth;  // Access globals from scsynth namespace

    if (!control || !shared_memory) return;

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
