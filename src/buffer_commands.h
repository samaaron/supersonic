/*
    SuperSonic - SuperCollider AudioWorklet WebAssembly port
    Copyright (c) 2025 Sam Aaron

    Based on SuperCollider by James McCartney and community
    GPL v3 or later
*/

#pragma once

#ifdef __EMSCRIPTEN__

#include "scsynth/include/plugin_interface/SC_SndBuf.h"

struct World;

// Buffer information structure for queries
typedef struct {
    int bufnum;
    int frames;
    int channels;
    int samples;
    double samplerate;
} buffer_info_t;

// Set buffer data from pre-allocated memory
// The data pointer points to interleaved audio already in SharedArrayBuffer
// Returns 0 on success, -1 on error
int buffer_set_data(
    World* world,
    int bufnum,
    float* data,        // Pointer to interleaved audio in SharedArrayBuffer
    int numFrames,
    int numChannels,
    double sampleRate
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

#endif // __EMSCRIPTEN__
