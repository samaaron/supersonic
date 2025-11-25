/*
    SuperSonic - SuperCollider AudioWorklet WebAssembly port
    Copyright (c) 2025 Sam Aaron

    Based on SuperCollider by James McCartney and community
    GPL v3 or later
*/

#ifndef SCSYNTH_AUDIO_PROCESSOR_H
#define SCSYNTH_AUDIO_PROCESSOR_H

#include <emscripten/emscripten.h>
#include <cstdint>
#include <cstdarg>
#include "shared_memory.h"

// scsynth forward declarations
struct World;
struct WorldOptions;
struct ReplyAddress;

extern "C" {
    // Static ring buffer (allocated in WASM data segment)
    // This is separate from scsynth heap, preventing memory conflicts
    // Size: ~2.2MB (IN: 768KB, OUT: 128KB, DEBUG: 64KB, control/metrics, node tree ~57KB, audio capture ~1.1MB)
    extern uint8_t ring_buffer_storage[TOTAL_BUFFER_SIZE];

    // Global state
    extern uint8_t* shared_memory;
    extern ControlPointers* control;
    extern PerformanceMetrics* metrics;
    extern bool memory_initialized;
    extern World* g_world;

    // Exported functions
    EMSCRIPTEN_KEEPALIVE int get_ring_buffer_base();
    EMSCRIPTEN_KEEPALIVE const BufferLayout* get_buffer_layout();
    EMSCRIPTEN_KEEPALIVE void init_memory(double sample_rate);
    EMSCRIPTEN_KEEPALIVE bool process_audio(double current_time);
    EMSCRIPTEN_KEEPALIVE int worklet_debug(const char* fmt, ...);
    EMSCRIPTEN_KEEPALIVE int worklet_debug_va(const char* fmt, va_list args);
    EMSCRIPTEN_KEEPALIVE uint32_t get_process_count();
    EMSCRIPTEN_KEEPALIVE uint32_t get_messages_processed();
    EMSCRIPTEN_KEEPALIVE uint32_t get_messages_dropped();
    EMSCRIPTEN_KEEPALIVE uint32_t get_status_flags();

    // scsynth audio output functions
    EMSCRIPTEN_KEEPALIVE uintptr_t get_audio_output_bus();
    EMSCRIPTEN_KEEPALIVE int get_audio_buffer_samples();
    EMSCRIPTEN_KEEPALIVE double get_time_offset();
}

// Helper functions
inline uint32_t next_index(uint32_t idx, uint32_t buffer_size);
inline uint32_t available_space(uint32_t head, uint32_t tail, uint32_t buffer_size);
inline bool is_buffer_full(uint32_t head, uint32_t tail, uint32_t buffer_size);

// OSC reply callback for scsynth (C++ linkage, outside extern "C")
extern "C++" void osc_reply_to_ring_buffer(ReplyAddress* addr, char* msg, int size);

#endif // SCSYNTH_AUDIO_PROCESSOR_H
