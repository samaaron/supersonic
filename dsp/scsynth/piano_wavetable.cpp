// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025-2026 Sam Aaron
#include "piano_wavetable.h"

#include "SC_World.h"
#include "SC_InterfaceTable.h"
#include "SC_Prototypes.h"
#include "sc_msg_iter.h"

#include <atomic>
#include <cmath>
#include <cstdint>
#include <new>

extern "C" void supersonic_set_piano_wavetable(const short* data, size_t count);

namespace {

// What the plugin plays from. Swapped on the audio thread, freed off it.
short*             g_table  = nullptr;
std::atomic<size_t> g_frames{0};

struct TableCmd {
    int    bufnum;      // -1: take the table away
    short* fresh;       // built in stage 2
    size_t frames;
    short* stale;       // the table this one replaced, freed in stage 4
};

// Non real time: read channel 0 of the buffer into 16-bit integers, the way
// the plugin's compiled-in table was stored.
bool stage2(World* world, void* data) {
    auto* cmd = static_cast<TableCmd*>(data);
    if (cmd->bufnum < 0) return true;
    const SndBuf* buf = World_GetNRTBuf(world, (uint32)cmd->bufnum);
    if (!buf || !buf->data || buf->frames <= 0) return true;
    const size_t frames = (size_t)buf->frames;
    short* table = new (std::nothrow) short[frames];
    if (!table) return true;
    const int stride = buf->channels > 0 ? buf->channels : 1;
    for (size_t i = 0; i < frames; ++i) {
        float x = buf->data[i * stride];
        if (x > 1.0f) x = 1.0f; else if (x < -1.0f) x = -1.0f;
        const long v = lrintf(x * 32768.0f);
        table[i] = (short)(v > 32767 ? 32767 : v);
    }
    cmd->fresh  = table;
    cmd->frames = frames;
    return true;
}

// Real time: the plugin reads the table pointer on this thread, so this is
// where it changes hands.
bool stage3(World*, void* data) {
    auto* cmd = static_cast<TableCmd*>(data);
    if (cmd->bufnum >= 0 && !cmd->fresh) return true;  // nothing built: leave it
    cmd->stale = g_table;
    g_table    = cmd->fresh;
    g_frames.store(cmd->frames, std::memory_order_relaxed);
    supersonic_set_piano_wavetable(g_table, cmd->frames);
    return true;
}

// Non real time: the table nobody reads any more.
bool stage4(World*, void* data) {
    auto* cmd = static_cast<TableCmd*>(data);
    delete[] cmd->stale;
    return true;   // /done
}

void cleanup(World*, void* data) { delete static_cast<TableCmd*>(data); }

} // namespace

extern "C" size_t supersonic_piano_wavetable_frames(void) {
    return g_frames.load(std::memory_order_relaxed);
}

extern "C" int supersonic_meth_piano_wavetable(World* inWorld, int inSize, char* inData,
                                               ReplyAddress* inReply) {
    static const char* kName = "/supersonic/piano/wavetable";
    sc_msg_iter msg(inSize, inData);
    const int bufnum = msg.geti(-1);

    if (bufnum >= 0) {
        // Refused here, on the asking thread, so the refusal is immediate and
        // carries the reason: a short table would be dropped by the plugin
        // silently, and silence is not an answer.
        const SndBuf* buf = (uint32)bufnum < inWorld->mNumSndBufs ? World_GetBuf(inWorld, (uint32)bufnum) : nullptr;
        if (!buf || !buf->data || buf->frames <= 0) {
            SendFailureWithIntValue(inReply, kName, "buffer is not allocated", bufnum);
            return 0;
        }
        if ((size_t)buf->frames < supersonic_piano_wavetable_min_frames()) {
            SendFailureWithIntValue(inReply, kName, "buffer is shorter than the piano table", bufnum);
            return 0;
        }
    }

    auto* cmd = new (std::nothrow) TableCmd{bufnum, nullptr, 0, nullptr};
    if (!cmd) {
        SendFailureWithIntValue(inReply, kName, "out of memory", bufnum);
        return 0;
    }
    (*inWorld->ft->fDoAsynchronousCommand)(inWorld, inReply, kName, cmd,
                                           stage2, stage3, stage4, cleanup, 0, nullptr);
    return 0;
}
