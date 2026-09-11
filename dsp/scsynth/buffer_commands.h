/*
    SuperSonic - SuperCollider AudioWorklet WebAssembly port
    Copyright (c) 2025 Sam Aaron

    Based on SuperCollider by James McCartney and community
    GPL v3 or later
*/

/*
 * The implementation is Rust (rust/supersonic-buffers/src/lib.rs). This header
 * is the C++ declaration of its three entry points; the C++ callers are
 * unchanged.
 *
 * Why these exist at all: /b_alloc allocates a buffer for the engine, and none
 * of these do. On web JavaScript decodes a sample and passes the block in; on
 * native the sample loader reads it on its own I/O thread. Both then need to
 * hand the engine memory it did not allocate.
 */
#pragma once
#include <stdint.h>

#include "synth/include/plugin_interface/SC_SndBuf.h"

struct World;

extern "C" {

// Buffer information structure for queries
typedef struct {
    int bufnum;
    int frames;
    int channels;
    int samples;
    double samplerate;
} buffer_info_t;

// The guard layout a client allocates around a sample, from the crate that
// applies it (rust/supersonic-buffers): frames before and after the audio.
uint32_t supersonic_buffer_guard_before(void);
uint32_t supersonic_buffer_guard_after(void);

// Where the guest's inbox is and how big (scsynth_dsp.cpp, set at dsp_new).
// /b_allocPtr names a sample by offset from here.
const uint8_t* supersonic_inbox_base(void);
uint32_t       supersonic_inbox_bytes(void);
// Non-zero when `ptr` lies inside the inbox: a range the client owns, which
// no free on this side may touch.
int            supersonic_inbox_contains(const void* ptr);
// The outbox likewise: the lane the guest writes and a client reads in place.
uint8_t*       supersonic_outbox_base(void);
uint32_t       supersonic_outbox_bytes(void);

// Set buffer data from pre-allocated memory.
// When hasGuardSamples is true (WASM/JS path), the data pointer includes
// guard samples: [GUARD_BEFORE * ch] [audio] [GUARD_AFTER * ch]
// and buf->data is advanced past the guard prefix.
// When false, data points directly at audio with no guard frames around it.
// Returns 0 on success, -1 on error
int buffer_set_data(
    World* world,
    int bufnum,
    float* data,
    int numFrames,
    int numChannels,
    double sampleRate,
    bool hasGuardSamples
);

// Read data into existing buffer (for /b_readPtr)
// Copies from source data into buffer at specified offset
// Returns 0 on success, -1 on error
int buffer_read_data(
    World* world,
    int bufnum,
    float* data,        // Source data to copy
    int numFrames,
    int numChannels,
    int bufStartFrame,  // Offset in buffer to write to
    double sampleRate
);

// Get buffer information (for queries)
// Returns 0 on success, -1 on error
int buffer_get_info(
    World* world,
    int bufnum,
    buffer_info_t* info
);
}
