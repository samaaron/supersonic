// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2025-2026 Sam Aaron
/*
 * buffer_commands.cpp — pointing a SndBuf at bytes the guest does not own.
 *
 * A sample arrives as an ASSET (clockwork dsp_api.h): the client decoded it
 * into the inbox lane and the audio thread handed us a pointer. This binds a
 * buffer to that pointer — no copy — and unbinds it when the buffer is
 * freed. The same binding serves /b_allocPtr, the web client's spelling of
 * the same hand-off. Ported from SuperSonic's buffer_commands.cpp, which the
 * clockwork port had left behind while keeping its header.
 *
 * GUARD FRAMES. Interpolating readers touch a few samples before frame 0 and
 * after the last, so a bound region is expected to have valid memory there:
 * 3 frames before, 1 after. A caller that laid the sample out that way says
 * hasGuardSamples and passes the start of the guarded region; a caller whose
 * payload already has that surround (an asset committed at the payload's own
 * offset inside a guarded slot) passes the payload and says false.
 */
#include "buffer_commands.h"
#include "synth/include/plugin_interface/SC_World.h"
#include "synth/include/plugin_interface/SC_SndBuf.h"
#include "synth/include/common/clz.h"

#include "synth/include/server/SC_WorldOptions.h"   // scprintf

#include <cstring>

static inline int32_t BUFMASK(int32_t x) { return (1 << (31 - CLZ(x))) - 1; }

#define GUARD_BEFORE 3   // frames before the start (cubic interpolation reaches back)
#define GUARD_AFTER  1   // frames after the end (linear interpolation reaches forward)

extern "C" uint32_t supersonic_buffer_guard_before(void) { return GUARD_BEFORE; }
extern "C" uint32_t supersonic_buffer_guard_after(void)  { return GUARD_AFTER; }

int buffer_set_data(World* world, int bufnum, float* data, int numFrames, int numChannels,
                    double sampleRate, bool hasGuardSamples) {
    if (!world || !data) return -1;
    if (bufnum < 0 || bufnum >= (int)world->mNumSndBufs) {
        scprintf("[buffer_set_data] invalid buffer number %d (max %d)\n", bufnum, (int)world->mNumSndBufs - 1);
        return -1;
    }
    if (numFrames <= 0 || numChannels <= 0) {
        scprintf("[buffer_set_data] invalid dimensions (frames %d, channels %d)\n", numFrames, numChannels);
        return -1;
    }
    SndBuf* nrtBuf = World_GetNRTBuf(world, bufnum);
    if (!nrtBuf) return -1;

    const int32_t numSamples = numFrames * numChannels;
    nrtBuf->data       = hasGuardSamples ? data + (GUARD_BEFORE * numChannels) : data;
    nrtBuf->channels   = numChannels;
    nrtBuf->frames     = numFrames;
    nrtBuf->samples    = numSamples;
    nrtBuf->mask       = BUFMASK(numSamples);   // for delay lines
    nrtBuf->mask1      = nrtBuf->mask - 1;      // for interpolating oscillators
    nrtBuf->samplerate = sampleRate;
    nrtBuf->sampledur  = 1.0 / sampleRate;
    nrtBuf->coord      = 0;
    nrtBuf->sndfile    = nullptr;

    // The RT mirror follows at once: this runs on the audio thread (dsp_asset,
    // /b_allocPtr), so there is no swap to stage.
    if (SndBuf* rtBuf = World_GetBuf(world, bufnum)) {
        *rtBuf = *nrtBuf;
        world->mSndBufUpdates[bufnum].writes++;
    }
    return 0;
}

int buffer_read_data(World* world, int bufnum, float* data, int numFrames, int numChannels,
                     int bufStartFrame, double /*sampleRate*/) {
    if (!world || !data) return -1;
    if (bufnum < 0 || bufnum >= (int)world->mNumSndBufs) return -1;
    SndBuf* buf = World_GetNRTBuf(world, bufnum);
    if (!buf || !buf->data) return -1;
    if (bufStartFrame < 0 || bufStartFrame >= buf->frames) return -1;
    if (numChannels != buf->channels) return -1;
    int framesToWrite = numFrames;
    const int framesAvailable = buf->frames - bufStartFrame;
    if (framesToWrite > framesAvailable) framesToWrite = framesAvailable;
    std::memcpy(buf->data + (size_t)bufStartFrame * numChannels, data,
                (size_t)framesToWrite * numChannels * sizeof(float));
    return 0;
}

int buffer_get_info(World* world, int bufnum, buffer_info_t* info) {
    if (!world || !info) return -1;
    if (bufnum < 0 || bufnum >= (int)world->mNumSndBufs) return -1;
    SndBuf* buf = World_GetNRTBuf(world, bufnum);
    if (!buf) return -1;
    info->bufnum     = bufnum;
    info->frames     = buf->frames;
    info->channels   = buf->channels;
    info->samples    = buf->samples;
    info->samplerate = buf->samplerate;
    return 0;
}
