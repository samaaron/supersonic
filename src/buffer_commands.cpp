/*
    SuperSonic - SuperCollider AudioWorklet WebAssembly port
    Copyright (c) 2025 Sam Aaron

    Based on SuperCollider by James McCartney and community
    GPL v3 or later
*/

#ifdef __EMSCRIPTEN__

#include "buffer_commands.h"
#include "scsynth/include/plugin_interface/SC_World.h"
#include "scsynth/include/plugin_interface/SC_SndBuf.h"
#include "scsynth/include/common/clz.h"
#include <string.h>
#include <stdio.h>

extern "C" {
    int worklet_debug(const char* fmt, ...);
}

// Calculate mask for power-of-2 buffer operations (delay lines)
// Same as original SuperCollider BUFMASK implementation
static inline int32_t BUFMASK(int32_t x) {
    return (1 << (31 - CLZ(x))) - 1;
}

// Guard sample constants
// BufRd needs guard samples for interpolation at buffer boundaries
#define GUARD_BEFORE 3  // Samples before start (for cubic interpolation)
#define GUARD_AFTER 1   // Samples after end (for linear interpolation)

int buffer_set_data(
    World* world,
    int bufnum,
    float* data,
    int numFrames,
    int numChannels,
    double sampleRate
) {
    // Validate parameters
    if (!world || !data) {
        worklet_debug("[buffer_set_data] Error: NULL world or data pointer\n");
        return -1;
    }

    if (bufnum < 0 || bufnum >= world->mNumSndBufs) {
        worklet_debug("[buffer_set_data] Error: Invalid buffer number %d (max: %d)\n",
               bufnum, world->mNumSndBufs - 1);
        return -1;
    }

    if (numFrames <= 0 || numChannels <= 0) {
        worklet_debug("[buffer_set_data] Error: Invalid dimensions (frames: %d, channels: %d)\n",
               numFrames, numChannels);
        return -1;
    }

    // Get NRT buffer first (commands execute in NRT context)
    SndBuf* nrtBuf = World_GetNRTBuf(world, bufnum);
    if (!nrtBuf) {
        worklet_debug("[buffer_set_data] Error: Failed to get NRT buffer %d\n", bufnum);
        return -1;
    }

    // Calculate total samples
    int32_t numSamples = numFrames * numChannels;

    // IMPORTANT: The data pointer includes guard samples
    // Layout: [GUARD_BEFORE samples] [actual audio data] [GUARD_AFTER samples]
    // We point buf->data at the start of the actual audio data (after guard samples)
    nrtBuf->data = data + (GUARD_BEFORE * numChannels);
    nrtBuf->channels = numChannels;
    nrtBuf->frames = numFrames;
    nrtBuf->samples = numSamples;

    // Calculate masks for delay lines and oscillators (power-of-2 optimization)
    nrtBuf->mask = BUFMASK(numSamples);      // for delay lines
    nrtBuf->mask1 = nrtBuf->mask - 1;            // for interpolating oscillators

    // Set sample rate info
    nrtBuf->samplerate = sampleRate;
    nrtBuf->sampledur = 1.0 / sampleRate;

    // Clear coord and sndfile (not used in WebAssembly context)
    nrtBuf->coord = 0;
    nrtBuf->sndfile = nullptr;

    // CRITICAL: Copy NRT buffer to RT buffer (like BufAllocCmd::Stage3)
    // UGens read from the RT buffer, not the NRT buffer!
    SndBuf* rtBuf = World_GetBuf(world, bufnum);
    if (rtBuf) {
        *rtBuf = *nrtBuf;
        world->mSndBufUpdates[bufnum].writes++;
    } else {
        worklet_debug("[buffer_set_data] Warning: Could not get RT buffer %d\n", bufnum);
    }

    return 0;
}

int buffer_read_data(
    World* world,
    int bufnum,
    float* data,
    int numFrames,
    int numChannels,
    int bufStartFrame,
    double sampleRate
) {
    // Validate parameters
    if (!world || !data) {
        worklet_debug("[buffer_read_data] Error: NULL world or data pointer\n");
        return -1;
    }

    if (bufnum < 0 || bufnum >= world->mNumSndBufs) {
        worklet_debug("[buffer_read_data] Error: Invalid buffer number %d\n", bufnum);
        return -1;
    }

    // Get buffer
    SndBuf* buf = World_GetNRTBuf(world, bufnum);
    if (!buf) {
        worklet_debug("[buffer_read_data] Error: Failed to get buffer %d\n", bufnum);
        return -1;
    }

    // Check if buffer has been allocated
    if (!buf->data) {
        worklet_debug("[buffer_read_data] Error: Buffer %d has no data allocated\n", bufnum);
        return -1;
    }

    // Validate buffer start frame
    if (bufStartFrame < 0 || bufStartFrame >= buf->frames) {
        worklet_debug("[buffer_read_data] Error: bufStartFrame %d out of range (0-%d)\n",
               bufStartFrame, buf->frames - 1);
        return -1;
    }

    // Check if channels match
    if (numChannels != buf->channels) {
        worklet_debug("[buffer_read_data] Error: Channel mismatch (source: %d, buffer: %d)\n",
               numChannels, buf->channels);
        return -1;
    }

    // Calculate how many frames we can actually write
    int framesToWrite = numFrames;
    int framesAvailable = buf->frames - bufStartFrame;
    if (framesToWrite > framesAvailable) {
        worklet_debug("[buffer_read_data] Warning: Truncating write from %d to %d frames\n",
               framesToWrite, framesAvailable);
        framesToWrite = framesAvailable;
    }

    // Copy interleaved data into buffer
    int samplesToWrite = framesToWrite * numChannels;
    int sampleOffset = bufStartFrame * numChannels;
    memcpy(buf->data + sampleOffset, data, samplesToWrite * sizeof(float));

    return 0;
}

int buffer_get_info(
    World* world,
    int bufnum,
    buffer_info_t* info
) {
    // Validate parameters
    if (!world || !info) {
        worklet_debug("[buffer_get_info] Error: NULL world or info pointer\n");
        return -1;
    }

    if (bufnum < 0 || bufnum >= world->mNumSndBufs) {
        worklet_debug("[buffer_get_info] Error: Invalid buffer number %d\n", bufnum);
        return -1;
    }

    // Get buffer
    SndBuf* buf = World_GetNRTBuf(world, bufnum);
    if (!buf) {
        worklet_debug("[buffer_get_info] Error: Failed to get buffer %d\n", bufnum);
        return -1;
    }

    // Fill info structure
    info->bufnum = bufnum;
    info->frames = buf->frames;
    info->channels = buf->channels;
    info->samples = buf->samples;
    info->samplerate = buf->samplerate;

    return 0;
}

#endif // __EMSCRIPTEN__
