/*
    SuperSonic - SuperCollider AudioWorklet WebAssembly port
    Copyright (c) 2025 Sam Aaron

    Based on SuperCollider by James McCartney and community
    GPL v3 or later
*/

/*
 * The three entry points of buffer_commands.cpp.
 *
 * Why these exist at all: /b_alloc allocates a buffer for the engine, and none
 * of these do. On web JavaScript decodes a sample and passes the block in; on
 * native the sample loader reads it on its own I/O thread. Both then need to
 * hand the engine memory it did not allocate.
 */
#pragma once
#include <stddef.h>
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

// The guard layout a client allocates around a sample, from the code that
// applies it (buffer_commands.cpp): frames before and after the audio.
uint32_t supersonic_buffer_guard_before(void);
uint32_t supersonic_buffer_guard_after(void);

// Where the guest's assets is and how big (scsynth_dsp.cpp, set at dsp_new).
// /b_allocPtr names a sample by offset from here.
const uint8_t* supersonic_assets_base(void);
uint32_t       supersonic_assets_bytes(void);
// Non-zero when `ptr` lies inside the assets area: a range the client owns, which
// no free on this side may touch.
int            supersonic_assets_contains(const void* ptr);
// The returns area likewise: the lane the guest writes and a client reads in place.
uint8_t*       supersonic_returns_base(void);
uint32_t       supersonic_returns_bytes(void);

// The guest's heap (scsynth_dsp.cpp): clockwork's heap when there is a host,
// nova's aligned malloc when there is none. Everything the server makes while
// sound plays — a buffer's samples, a synth definition's tables, the piano
// wavetable — is kept here and never with new or malloc, which clockwork keeps
// off the audio thread. 16-aligned; NULL for 0 bytes, or when the heap is out.
void*          supersonic_guest_alloc_bytes(size_t bytes);
void           supersonic_guest_free_bytes(void* ptr);

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
