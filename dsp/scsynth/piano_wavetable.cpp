// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025-2026 Sam Aaron
#include "piano_wavetable.h"

#include "SC_World.h"
#include "SC_InterfaceTable.h"
#include "SC_Prototypes.h"
#include "sc_msg_iter.h"
#include "guest_doors.h"   // the instance's table, and its heap
#include "guest_heap.h"

#include <cmath>
#include <cstdint>
#include <new>

namespace {

struct TableCmd {
    int    bufnum;      // -1: take the table away
    short* fresh;       // built in stage 2
    size_t frames;
    short* stale;       // the table this one replaced, handed back to the heap in stage 4
};

// Non real time: read channel 0 of the buffer into 16-bit integers, the way
// the plugin's compiled-in table was stored.
bool stage2(World* world, void* data) {
    auto* cmd = static_cast<TableCmd*>(data);
    if (cmd->bufnum < 0) return true;
    const SndBuf* buf = World_GetNRTBuf(world, (uint32)cmd->bufnum);
    if (!buf || !buf->data || buf->frames <= 0) return true;
    const size_t frames = (size_t)buf->frames;
    short* table = static_cast<short*>(supersonic_guest_alloc_bytes(world, frames * sizeof(short)));
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
bool stage3(World* world, void* data) {
    auto* cmd = static_cast<TableCmd*>(data);
    if (cmd->bufnum >= 0 && !cmd->fresh) return true;  // nothing built: leave it
    cmd->stale = supersonic_piano_wavetable_swap(world, cmd->fresh, cmd->frames);
    return true;
}

// Non real time: the table nobody reads any more.
bool stage4(World* world, void* data) {
    auto* cmd = static_cast<TableCmd*>(data);
    supersonic_guest_free_bytes(world, cmd->stale);
    return true;   // /done
}

void cleanup(World* world, void* data) { guest_delete(world, static_cast<TableCmd*>(data)); }

} // namespace

extern "C" int supersonic_meth_piano_wavetable(World* inWorld, int inSize, const char* inData,
                                               ReplyAddress* inReply) {
    constexpr const char* kName = "/supersonic/piano/wavetable";
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

    // The command's record from the instance's heap, as everything the server
    // makes is (guest_heap.h): the system allocator is off the audio thread.
    void* mem = supersonic_guest_alloc_bytes(inWorld, sizeof(TableCmd));
    if (!mem) {
        SendFailureWithIntValue(inReply, kName, "out of memory", bufnum);
        return 0;
    }
    auto* cmd = new (mem) TableCmd{bufnum, nullptr, 0, nullptr};
    (*inWorld->ft->fDoAsynchronousCommand)(inWorld, inReply, kName, cmd,
                                           stage2, stage3, stage4, cleanup, 0, nullptr);
    return 0;
}
