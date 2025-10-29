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
#include "scheduler/PriorityQueue.h"
#include "scheduler/SC_ScheduledEvent.h"

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
static const int SUPERSONIC_VERSION_MINOR = 1;
static const int SUPERSONIC_VERSION_PATCH = 0;

// Global pointers
extern "C" {
    // Static ring buffer allocated in WASM data segment
    // This ensures no conflicts with scsynth heap allocations
    alignas(4) uint8_t ring_buffer_storage[65536];

    // Validate at compile time that buffer layout fits in allocated storage
    static_assert(TOTAL_BUFFER_SIZE <= sizeof(ring_buffer_storage),
                  "Buffer layout exceeds allocated storage!");

    // Static audio bus buffer (128 channels * 128 samples * 4 bytes = 64KB)
    // Pre-allocated to avoid malloc in critical audio path
    alignas(16) float static_audio_bus[128 * 128];

    uint8_t* shared_memory = nullptr;
    ControlPointers* control = nullptr;
    PerformanceMetrics* metrics = nullptr;
    bool memory_initialized = false;
    World* g_world = nullptr;

    // OSC Bundle Scheduler - Priority queue for timed bundle execution
    // Based on SC_CoreAudio.h:159 - Uses static array for RT-safety (no malloc)
    PriorityQueueT<SC_ScheduledEvent, 64> g_scheduler;

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

        // Create event with embedded data (no malloc!)
        SC_ScheduledEvent event(world, ntp_time, data, size, true, reply_addr);

        // Add to scheduler (copies event into queue)
        if (!g_scheduler.Add(event)) {
            worklet_debug("ERROR: Scheduler queue full (%d events)", g_scheduler.Size());
            return false;
        }

        // Debug timing logs (commented out - enable for scheduler debugging)
        // int64_t time_diff_osc = ntp_time - current_osc_time;
        // double time_diff_ms = ((double)time_diff_osc / 4294967296.0) * 1000.0;
        // worklet_debug("SCHED: Bundle scheduled - timetag=%llu current=%llu diff=%.2fms size=%d queue=%d",
        //              (unsigned long long)ntp_time, (unsigned long long)current_osc_time,
        //              time_diff_ms, size, g_scheduler.Size());

        return true;
    }

    // Initialize memory pointers using the static ring buffer
    EMSCRIPTEN_KEEPALIVE
    void init_memory(double sample_rate) {
        shared_memory = ring_buffer_storage;
        control = reinterpret_cast<ControlPointers*>(shared_memory + CONTROL_START);
        metrics = reinterpret_cast<PerformanceMetrics*>(shared_memory + METRICS_START);

        // Initialize all atomics to 0
        control->in_head.store(0, std::memory_order_relaxed);
        control->in_tail.store(0, std::memory_order_relaxed);
        control->out_head.store(0, std::memory_order_relaxed);
        control->out_tail.store(0, std::memory_order_relaxed);
        control->debug_head.store(0, std::memory_order_relaxed);
        control->debug_tail.store(0, std::memory_order_relaxed);
        control->sequence.store(0, std::memory_order_relaxed);
        control->status_flags.store(STATUS_OK, std::memory_order_relaxed);

        // Initialize metrics
        metrics->process_count.store(0, std::memory_order_relaxed);
        metrics->buffer_overruns.store(0, std::memory_order_relaxed);
        metrics->messages_processed.store(0, std::memory_order_relaxed);
        metrics->messages_dropped.store(0, std::memory_order_relaxed);

        // Enable worklet_debug
        memory_initialized = true;

        worklet_debug("Booting SuperSonic %d.%d.%d (scsynth-nrt %d.%d.%d%s)",
                     SUPERSONIC_VERSION_MAJOR, SUPERSONIC_VERSION_MINOR, SUPERSONIC_VERSION_PATCH,
                     SC_VersionMajor, SC_VersionMinor, SC_VersionPatch, SC_VersionTweak);

        // Configure World for NRT mode (externally driven by AudioWorklet)
        WorldOptions options;
        options.mRealTime = false;                    // NRT mode - externally driven, no audio driver
        options.mMemoryLocking = false;               // No memory locking in WASM
        options.mNumAudioBusChannels = 128;
        options.mNumControlBusChannels = 4096;
        options.mNumInputBusChannels = 0;             // No hardware input
        options.mNumOutputBusChannels = 2;            // Stereo output
        options.mBufLength = 128;                     // Match AudioWorklet quantum size (128 samples)
        options.mPreferredSampleRate = (uint32_t)sample_rate;
        options.mNumBuffers = 1024;                   // SndBuf count
        options.mMaxNodes = 1024;
        options.mMaxGraphDefs = 1024;
        options.mMaxWireBufs = 64;                    // Wire buffers for routing audio between UGens
        options.mRealTimeMemorySize = 16384;          // 16 MB for AllocPool (default is 8MB)
        options.mLoadGraphDefs = 0;                   // Don't auto-load SynthDefs from disk

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
        g_scheduler.Empty();

        worklet_debug("Scheduler initialized: buf=%d samples, osc_inc=%lld",
                     buf_length, (long long)g_osc_increment);

        worklet_debug("░█▀▀░█░█░█▀█░█▀▀░█▀▄░█▀▀░█▀█░█▀█░▀█▀░█▀▀");
        worklet_debug("░▀▀█░█░█░█▀▀░█▀▀░█▀▄░▀▀█░█░█░█░█░░█░░█░░");
        worklet_debug("░▀▀▀░▀▀▀░▀░░░▀▀▀░▀░▀░▀▀▀░▀▀▀░▀░▀░▀▀▀░▀▀▀");
        worklet_debug("scsynth-nrt ready: %.0fHz, %d channels (build: %s)", sample_rate, options.mNumOutputBusChannels, BUILD_HASH);
    }

    // Main audio processing function - called every audio frame (128 samples)
    EMSCRIPTEN_KEEPALIVE
    bool process_audio(double current_time, double unix_seconds) {
        if (!memory_initialized) {
            return true; // Keep alive but do nothing if not initialized
        }

        if (!metrics) {
            return false;
        }

        // Time offset is set by JavaScript via set_time_offset()
        // No longer calculated here to avoid timing discrepancies

        metrics->process_count.fetch_add(1, std::memory_order_relaxed);

        // Process incoming OSC messages if scsynth is ready
        if (g_world) {
            int32_t in_head = control->in_head.load(std::memory_order_acquire);
            int32_t in_tail = control->in_tail.load(std::memory_order_acquire);

            // Process all available messages
            while (in_head != in_tail) {
                // Read message header - now always contiguous due to padding
                uint32_t msg_offset = IN_BUFFER_START + in_tail;
                uint32_t space_to_end = IN_BUFFER_SIZE - in_tail;

                Message header;
                std::memcpy(&header, shared_memory + msg_offset, sizeof(Message));

                // Check for padding marker - skip to beginning
                if (header.magic == PADDING_MAGIC) {
                    control->in_tail.store(0, std::memory_order_release);
                    in_tail = 0;
                    continue;
                }

                // Validate message
                if (header.magic != MESSAGE_MAGIC) {
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

                char osc_buffer[MAX_MESSAGE_SIZE];

                // Copy OSC payload from ring buffer (contiguous due to padding)
                uint32_t payload_offset = IN_BUFFER_START + in_tail + sizeof(Message);
                std::memcpy(osc_buffer, shared_memory + payload_offset, payload_size);

                // RT-SAFE message processing - no malloc!
                // Setup reply address
                ReplyAddress reply_addr;
                reply_addr.mProtocol = kWeb;
                reply_addr.mReplyFunc = osc_reply_to_ring_buffer;
                reply_addr.mReplyData = nullptr;

                bool is_bundle_msg = is_bundle(osc_buffer, payload_size);

                // Debug: Log what type of message we received (commented out - enable for message debugging)
                // worklet_debug("MSG: Received %s (size=%d)",
                //              is_bundle_msg ? "BUNDLE" : "MESSAGE", payload_size);

                if (is_bundle_msg) {
                    // Extract timetag
                    uint64_t timetag = extract_timetag(osc_buffer);

                    // Check if immediate execution (timetag == 0 or 1)
                    if (timetag == 0 || timetag == 1) {
                        // Immediate bundle - execute now
                        worklet_debug("BUNDLE: Immediate execution (timetag=%llu)", (unsigned long long)timetag);
                        OSC_Packet packet;
                        packet.mData = osc_buffer;
                        packet.mSize = payload_size;
                        packet.mIsBundle = true;
                        packet.mReplyAddr = reply_addr;

                        PerformOSCBundle(g_world, &packet);
                    } else {
                        // Future bundle - schedule it (RT-safe, no malloc!)
                        // Calculate current OSC time for logging
                        int64_t current_osc_time = audio_to_osc_time(current_time);
                        // Cast uint64_t to int64 for scheduler (SC uses signed int64)
                        if (!schedule_bundle(g_world, (int64_t)timetag, current_osc_time, osc_buffer, payload_size, reply_addr)) {
                            worklet_debug("ERROR: Failed to schedule bundle");
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
            // Convert current time to OSC/NTP format
            int64_t currentOscTime = audio_to_osc_time(current_time);
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

                SC_ScheduledEvent event = g_scheduler.Remove();

                // Late bundle detection - warn when timing is broken
                int64_t time_diff_osc = schedTime - currentOscTime;
                double time_diff_ms = ((double)time_diff_osc / 4294967296.0) * 1000.0;

                // Warn if bundle is late (>3ms) or negative (indicates broken timing)
                if (time_diff_ms > 3.0 || time_diff_ms < 0.0) {
                    worklet_debug("LATE: Bundle executing - timetag=%llu current=%llu diff=%.2fms offset=%d subsample=%.3f",
                                 (unsigned long long)schedTime, (unsigned long long)currentOscTime,
                                 time_diff_ms, g_world->mSampleOffset, g_world->mSubsampleOffset);
                }

                event.Perform();  // Calls PerformOSCBundle + no free (embedded data)
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
    header.sequence = control->sequence.fetch_add(1, std::memory_order_relaxed);

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
            metrics->buffer_overruns.fetch_add(1, std::memory_order_relaxed);
        }
        if (control) {
            control->status_flags.fetch_or(STATUS_BUFFER_FULL, std::memory_order_relaxed);
        }
        return false;
    }

    // Check if message fits contiguously, otherwise write padding and wrap
    uint32_t space_to_end = buffer_size - current_head;
    if (header.length > space_to_end) {
        // Write padding marker to fill remaining space
        Message padding;
        padding.magic = PADDING_MAGIC;
        padding.length = 0;
        padding.sequence = 0;

        std::memcpy(buffer_start + buffer_start_offset + current_head, &padding, sizeof(Message));

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
