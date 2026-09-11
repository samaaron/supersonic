/*
    SuperCollider real time audio synthesis system
    Copyright (c) 2002 James McCartney. All rights reserved.
    http://www.audiosynth.com

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301  USA
*/


#include "SC_Lib.h"
#include "SC_CoreAudio.h"
#include "SC_HiddenWorld.h"
#include "SC_Graph.h"
#include "SC_GraphDef.h"
#include "SC_Group.h"
#include "SC_UnitDef.h"
#include <stdexcept>
#include "SC_Lib_Cintf.h"
#include "SC_SequencedCommand.h"
#include <new>
#include "SC_Prototypes.h"
#include "scsynthsend.h"
#include "SC_WorldOptions.h"
#include "SC_Version.hpp"
#include "clock/ClockworkClock.h"

extern int gMissingNodeID;

// From audio_processor.cpp
extern "C" {
    int clockwork_log(const char* fmt, ...);
    extern uint8_t* shared_memory;
}

// =============================================================================
// TAU MODIFICATION START - Direct SAB tree update for /n_order
// When nodes are reordered within the same group, SuperCollider doesn't send
// kNode_Move (no OSC notification). But we need to update the SAB tree.
// This helper updates the SAB tree directly without triggering OSC messages.
// =============================================================================
#include "../../node_tree.h"

static void NodeTree_UpdateDirect(Node* node) {
    if (!shared_memory || !node) return;

    NodeTreeHeader* tree_header  = supersonic_node_tree_header();
    NodeEntry*      tree_entries = supersonic_node_tree_entries();
    if (!tree_header || !tree_entries) return;

    NodeTree_Update(node, tree_header, tree_entries);
}
// =============================================================================
// TAU MODIFICATION END
// =============================================================================

// returns number of bytes in an OSC string.
int OSCstrlen(char* strin);

Node* Msg_GetNode(World* inWorld, sc_msg_iter& msg) {
    Node* node;
    if (msg.nextTag('i') == 's') {
        const char* loc = msg.gets();
        int32 nodeID = msg.geti();
        gMissingNodeID = nodeID;
        node = World_GetNode(inWorld, nodeID);
        while (*loc) {
            if (!node)
                return nullptr;
            switch (*loc) {
            case 'h':
                if (!node->mIsGroup)
                    return nullptr;
                node = ((Group*)node)->mHead;
                break;
            case 't':
                if (!node->mIsGroup)
                    return nullptr;
                node = ((Group*)node)->mTail;
                break;
            case 'u':
                node = &node->mParent->mNode;
                break;
            case 'p':
                node = node->mPrev;
                break;
            case 'n':
                node = node->mNext;
                break;
            }
            loc++;
        }
    } else {
        int32 nodeID = msg.geti();
        gMissingNodeID = nodeID;
        node = World_GetNode(inWorld, nodeID);
    }
    return node;
}

Group* Msg_GetGroup(World* inWorld, sc_msg_iter& msg) {
    Node* node = Msg_GetNode(inWorld, msg);
    return node && node->mIsGroup ? (Group*)node : nullptr;
}

Graph* Msg_GetGraph(World* inWorld, sc_msg_iter& msg) {
    Node* node = Msg_GetNode(inWorld, msg);
    return !node || node->mIsGroup ? nullptr : (Graph*)node;
}

SCErr meth_none(World* inWorld, int inSize, char* inData, ReplyAddress* inReply);
SCErr meth_none(World* inWorld, int inSize, char* inData, ReplyAddress* inReply) { return kSCErr_None; }

SCErr meth_sync(World* inWorld, int inSize, char* inData, ReplyAddress* inReply);
SCErr meth_sync(World* inWorld, int inSize, char* inData, ReplyAddress* inReply) {
    CallSequencedCommand(SyncCmd, inWorld, inSize, inData, inReply);
    return kSCErr_None;
}

SCErr meth_b_alloc(World* inWorld, int inSize, char* inData, ReplyAddress* inReply);
SCErr meth_b_alloc(World* inWorld, int inSize, char* inData, ReplyAddress* inReply) {
    CallSequencedCommand(BufAllocCmd, inWorld, inSize, inData, inReply);
    return kSCErr_None;
}

SCErr meth_b_free(World* inWorld, int inSize, char* inData, ReplyAddress* inReply);
SCErr meth_b_free(World* inWorld, int inSize, char* inData, ReplyAddress* inReply) {
    CallSequencedCommand(BufFreeCmd, inWorld, inSize, inData, inReply);

    return kSCErr_None;
}

SCErr meth_b_close(World* inWorld, int inSize, char* inData, ReplyAddress* inReply);
SCErr meth_b_close(World* inWorld, int inSize, char* inData, ReplyAddress* inReply) {
    CallSequencedCommand(BufCloseCmd, inWorld, inSize, inData, inReply);

    return kSCErr_None;
}

SCErr meth_b_allocRead(World* inWorld, int inSize, char* inData, ReplyAddress* inReply);
SCErr meth_b_allocRead(World* inWorld, int inSize, char* inData, ReplyAddress* inReply) {
    CallSequencedCommand(BufAllocReadCmd, inWorld, inSize, inData, inReply);
    return kSCErr_None;
}

SCErr meth_b_read(World* inWorld, int inSize, char* inData, ReplyAddress* inReply);
SCErr meth_b_read(World* inWorld, int inSize, char* inData, ReplyAddress* inReply) {
    CallSequencedCommand(BufReadCmd, inWorld, inSize, inData, inReply);

    return kSCErr_None;
}

SCErr meth_b_allocReadChannel(World* inWorld, int inSize, char* inData, ReplyAddress* inReply);
SCErr meth_b_allocReadChannel(World* inWorld, int inSize, char* inData, ReplyAddress* inReply) {
    CallSequencedCommand(BufAllocReadChannelCmd, inWorld, inSize, inData, inReply);

    return kSCErr_None;
}

SCErr meth_b_readChannel(World* inWorld, int inSize, char* inData, ReplyAddress* inReply);
SCErr meth_b_readChannel(World* inWorld, int inSize, char* inData, ReplyAddress* inReply) {
    CallSequencedCommand(BufReadChannelCmd, inWorld, inSize, inData, inReply);

    return kSCErr_None;
}

SCErr meth_b_write(World* inWorld, int inSize, char* inData, ReplyAddress* inReply);
SCErr meth_b_write(World* inWorld, int inSize, char* inData, ReplyAddress* inReply) {
    CallSequencedCommand(BufWriteCmd, inWorld, inSize, inData, inReply);

    return kSCErr_None;
}

SCErr meth_b_zero(World* inWorld, int inSize, char* inData, ReplyAddress* inReply);
SCErr meth_b_zero(World* inWorld, int inSize, char* inData, ReplyAddress* inReply) {
    CallSequencedCommand(BufZeroCmd, inWorld, inSize, inData, inReply);
    return kSCErr_None;
}

// /b_allocPtr bufnum inboxOffset numFrames numChannels sampleRate uuid [completion]
// Point a buffer at a sample the client wrote into the inbox, in place: no copy.
// inboxOffset is measured from the inbox base, on every platform, and the range
// carries the guard frames the client laid around the audio.
#include "buffer_commands.h"
#include "piano_wavetable.h"
#include "shm_audio_buffer.hpp"

SCErr meth_b_allocPtr(World* inWorld, int inSize, char* inData, ReplyAddress* inReply);
SCErr meth_b_allocPtr(World* inWorld, int inSize, char* inData, ReplyAddress* inReply) {
    sc_msg_iter msg(inSize, inData);

    int bufnum = msg.geti();
    int inboxOffset = msg.geti();  // an OFFSET from the inbox base, never an address
    int numFrames = msg.geti();
    int numChannels = msg.geti();
    float sampleRate = msg.getf();
    const char* uuid = msg.gets();  // UUID for correlation

    // The sample lives in the inbox — the lane the client writes and this
    // engine reads in place — and the client names it by offset, because an
    // address would be right on the web (one linear memory) and wrong natively
    // (the engine maps the lane wherever it likes) without ever faulting.
    // The range is [guard][audio][guard], sized from one definition of the
    // guard layout, and a range that does not fit the lane is refused rather
    // than read.
    const uint8_t* inbox = supersonic_inbox_base();
    const uint64_t frames = (uint64_t)supersonic_buffer_guard_before()
                          + (uint64_t)(numFrames > 0 ? numFrames : 0)
                          + (uint64_t)supersonic_buffer_guard_after();
    const uint64_t bytes  = frames * (uint64_t)(numChannels > 0 ? numChannels : 0) * sizeof(float);
    if (!inbox || inboxOffset < 0 || numFrames <= 0 || numChannels <= 0
        || (uint64_t)inboxOffset + bytes > (uint64_t)supersonic_inbox_bytes()) {
        SendFailureWithIntValue(inReply, "/b_allocPtr", "range is not inside the inbox", bufnum);
        return kSCErr_None;
    }
    float* data = reinterpret_cast<float*>(const_cast<uint8_t*>(inbox) + inboxOffset);

    // Call buffer_set_data to configure the buffer (with guard samples)
    int result = buffer_set_data(inWorld, bufnum, data, numFrames, numChannels, sampleRate, true);

    if (result == 0) {
        SendDoneWithIntValue(inReply, "/b_allocPtr", bufnum);

        // Send internal message for host to track buffer allocation
        small_scpacket packet;
        packet.adds("/supersonic/buffer/allocated");
        packet.maketags(3);
        packet.addtag(',');
        packet.addtag('s');  // UUID string
        packet.addtag('i');  // buffer number
        packet.adds(uuid);
        packet.addi(bufnum);

        SendReply(inReply, packet.data(), packet.size());
    } else {
        SendFailureWithIntValue(inReply, "/b_allocPtr", "Buffer allocation failed", bufnum);
    }

    return kSCErr_None;
}

// /supersonic/buffer/read bufnum bufOffset inboxOffset numFrames numChannels sampleRate
//
// Frames from the inbox lane into an ALLOCATED buffer, in place of what
// /b_read did with a file: the client decoded them (clockwork_audio_file.h
// is a client API) and laid them out at inboxOffset, interleaved, with no
// guard frames — the buffer keeps its own. Clamped to the buffer's end, as
// /b_read clamped; refused when the channel counts differ, as /b_read
// refused; the rate, if given, becomes the buffer's, as /b_read set it.
SCErr meth_supersonic_buffer_read(World* inWorld, int inSize, char* inData, ReplyAddress* inReply);
SCErr meth_supersonic_buffer_read(World* inWorld, int inSize, char* inData, ReplyAddress* inReply) {
    static const char* kName = "/supersonic/buffer/read";
    sc_msg_iter msg(inSize, inData);
    const int bufnum      = msg.geti();
    const int bufOffset   = msg.geti();
    const int inboxOffset = msg.geti();
    const int numFrames   = msg.geti();
    const int numChannels = msg.geti();
    const float rate      = msg.getf();

    if (bufnum < 0 || (uint32)bufnum >= inWorld->mNumSndBufs) {
        SendFailureWithIntValue(inReply, kName, "buffer number out of range", bufnum);
        return kSCErr_None;
    }
    SndBuf* buf = World_GetNRTBuf(inWorld, bufnum);
    if (!buf->data || buf->channels <= 0) {
        SendFailureWithIntValue(inReply, kName, "buffer is not allocated", bufnum);
        return kSCErr_None;
    }
    if (numChannels != buf->channels) {
        char str[256];
        snprintf(str, sizeof str, "Channel mismatch. Lane has %d channels. Buffer has %d channels.\n",
                 numChannels, buf->channels);
        SendFailureWithIntValue(inReply, kName, str, bufnum);
        return kSCErr_None;
    }
    const uint8_t* inbox = supersonic_inbox_base();
    const uint64_t bytes = (uint64_t)(numFrames > 0 ? numFrames : 0) * (uint64_t)numChannels * sizeof(float);
    if (!inbox || inboxOffset < 0 || numFrames < 0
        || (uint64_t)inboxOffset + bytes > (uint64_t)supersonic_inbox_bytes()) {
        SendFailureWithIntValue(inReply, kName, "range is not inside the inbox", bufnum);
        return kSCErr_None;
    }
    const int framesToEnd = buf->frames - (bufOffset < 0 ? 0 : bufOffset);
    const int n = framesToEnd <= 0 ? 0 : (numFrames < framesToEnd ? numFrames : framesToEnd);
    if (n > 0 && bufOffset >= 0)
        memcpy(buf->data + (size_t)bufOffset * buf->channels, inbox + inboxOffset,
               (size_t)n * buf->channels * sizeof(float));
    if (rate > 0.f) {
        buf->samplerate = rate;
        World_GetBuf(inWorld, bufnum)->samplerate = rate;
    }
    inWorld->mSndBufUpdates[bufnum].writes++;
    SendDoneWithIntValue(inReply, kName, bufnum);
    return kSCErr_None;
}

// /supersonic/buffer/publish bufnum bufOffset numFrames outboxOffset
//
// A buffer's frames into the outbox lane, for a client to encode — in place
// of what /b_write did with a file. Clamped to the buffer's end (numFrames
// < 0: to the end), as /b_write clamped. The reply says what landed:
// /supersonic/buffer/published bufnum channels frames rate.
SCErr meth_supersonic_buffer_publish(World* inWorld, int inSize, char* inData, ReplyAddress* inReply);
SCErr meth_supersonic_buffer_publish(World* inWorld, int inSize, char* inData, ReplyAddress* inReply) {
    static const char* kName = "/supersonic/buffer/publish";
    sc_msg_iter msg(inSize, inData);
    const int bufnum       = msg.geti();
    const int bufOffset    = msg.geti();
    const int numFrames    = msg.geti(-1);
    const int outboxOffset = msg.geti();

    if (bufnum < 0 || (uint32)bufnum >= inWorld->mNumSndBufs) {
        SendFailureWithIntValue(inReply, kName, "buffer number out of range", bufnum);
        return kSCErr_None;
    }
    const SndBuf* buf = World_GetBuf(inWorld, bufnum);
    if (!buf->data || buf->channels <= 0) {
        SendFailureWithIntValue(inReply, kName, "buffer is not allocated", bufnum);
        return kSCErr_None;
    }
    const int start = bufOffset < 0 ? 0 : bufOffset;
    int framesToEnd = buf->frames - start;
    if (framesToEnd < 0) framesToEnd = 0;
    const int n = numFrames < 0 || numFrames > framesToEnd ? framesToEnd : numFrames;
    uint8_t* outbox = supersonic_outbox_base();
    const uint64_t bytes = (uint64_t)n * (uint64_t)buf->channels * sizeof(float);
    if (!outbox || outboxOffset < 0 || (uint64_t)outboxOffset + bytes > (uint64_t)supersonic_outbox_bytes()) {
        SendFailureWithIntValue(inReply, kName, "range is not inside the outbox", bufnum);
        return kSCErr_None;
    }
    if (n > 0)
        memcpy(outbox + outboxOffset, buf->data + (size_t)start * buf->channels, (size_t)bytes);

    small_scpacket packet;
    packet.adds("/supersonic/buffer/published");
    packet.maketags(5);
    packet.addtag(',');
    packet.addtag('i');
    packet.addtag('i');
    packet.addtag('i');
    packet.addtag('f');
    packet.addi(bufnum);
    packet.addi(buf->channels);
    packet.addi(n);
    packet.addf((float)buf->samplerate);
    SendReply(inReply, packet.data(), packet.size());
    return kSCErr_None;
}

SCErr meth_u_cmd(World* inWorld, int inSize, char* inData, ReplyAddress* inReply);
SCErr meth_u_cmd(World* inWorld, int inSize, char* inData, ReplyAddress* inReply) {
    return Unit_DoCmd(inWorld, inSize, inData, inReply);
};

SCErr meth_cmd(World* inWorld, int inSize, char* inData, ReplyAddress* inReply);
SCErr meth_cmd(World* inWorld, int inSize, char* inData, ReplyAddress* inReply) {
    return PlugIn_DoCmd(inWorld, inSize, inData, inReply);
};


/*
SCErr meth_n_cmd(World *inWorld, int inSize, char *inData, ReplyAddress *inReply);
SCErr meth_n_cmd(World *inWorld, int inSize, char *inData, ReplyAddress *inReply)
{
    sc_msg_iter msg(inSize, inData);
    Node *node = Msg_GetNode(inWorld, msg);
    if (!node) return kSCErr_NodeNotFound;

    char *args = msg.rdpos;
    int arglen = msg.remain();

//!!	(node->mDef->fNodeCmd)(node, arglen, args);

    return kSCErr_None;
}
*/

SCErr meth_n_trace(World* inWorld, int inSize, char* inData, ReplyAddress* inReply);
SCErr meth_n_trace(World* inWorld, int inSize, char* inData, ReplyAddress* /*inReply*/) {
    sc_msg_iter msg(inSize, inData);
    while (msg.remain()) {
        Node* node = Msg_GetNode(inWorld, msg);
        if (!node)
            return kSCErr_NodeNotFound;

        Node_Trace(node);
    }

    return kSCErr_None;
}

SCErr meth_g_dumpTree(World* inWorld, int inSize, char* inData, ReplyAddress* inReply);
SCErr meth_g_dumpTree(World* inWorld, int inSize, char* inData, ReplyAddress* /*inReply*/) {
    sc_msg_iter msg(inSize, inData);
    while (msg.remain()) {
        Group* group = Msg_GetGroup(inWorld, msg);
        if (!group)
            return kSCErr_GroupNotFound;
        int32 flag = msg.geti();
        if (flag) {
            Group_DumpTreeAndControls(group);
        } else {
            Group_DumpTree(group);
        }
    }

    return kSCErr_None;
}

// SCErr meth_g_queryTree(World *inWorld, int inSize, char *inData, ReplyAddress *inReply);
// SCErr meth_g_queryTree(World *inWorld, int inSize, char *inData, ReplyAddress* inReply)
//{
//	sc_msg_iter msg(inSize, inData);
//	while (msg.remain()) {
//		Group *group = Msg_GetGroup(inWorld, msg);
//		if (!group) return kSCErr_GroupNotFound;
//
//		// first count the total number of nodes to know how many tags the packet should have
//		int numNodes = 1; // include this one
//
//		Group_CountNodes(group, &numNodes);
//
//		big_scpacket packet;
//		packet.adds("/g_queryTree.reply");
//		packet.maketags(numNodes * 3 + 1);
//		packet.addtag(',');
//
//		Group_QueryTree(group, &packet);
//
//		if (packet.size()) {
//			CallSequencedCommand(SendReplyCmd, inWorld, packet.size(), packet.data(), inReply);
//		}
//	}
//	return kSCErr_None;
//}

SCErr meth_g_queryTree(World* inWorld, int inSize, char* inData, ReplyAddress* inReply);
SCErr meth_g_queryTree(World* inWorld, int inSize, char* inData, ReplyAddress* inReply) {
    sc_msg_iter msg(inSize, inData);
    while (msg.remain()) {
        Group* group = Msg_GetGroup(inWorld, msg);
        if (!group)
            return kSCErr_GroupNotFound;
        big_scpacket packet;
        packet.adds("/g_queryTree.reply");
        int32 flag = msg.geti();
        if (flag) {
            // first count the total number of nodes to know how many tags the packet should have
            int numNodes = 1; // include this one
            int numControlsAndDefs = 0;
            Group_CountNodeAndControlTags(group, &numNodes, &numControlsAndDefs);
            // nodeID and numChildren + numControlsAndDefs + controlFlag
            packet.maketags(numNodes * 2 + numControlsAndDefs + 2);
            packet.addtag(',');
            packet.addtag('i');
            packet.addi(1); // include controls flag
            Group_QueryTreeAndControls(group, &packet);
        } else {
            // first count the total number of nodes to know how many tags the packet should have
            int numNodeTags = 2; // include this one
            Group_CountNodeTags(group, &numNodeTags);
            packet.maketags(numNodeTags + 2); // nodeID and numChildren
            packet.addtag(',');
            packet.addtag('i');
            packet.addi(0); // include controls flag
            Group_QueryTree(group, &packet);
        }

        if (packet.size()) {
            CallSequencedCommand(SendReplyCmd, inWorld, packet.size(), packet.data(), inReply);
        }
    }
    return kSCErr_None;
}


SCErr meth_n_run(World* inWorld, int inSize, char* inData, ReplyAddress* inReply);
SCErr meth_n_run(World* inWorld, int inSize, char* inData, ReplyAddress* /*inReply*/) {
    sc_msg_iter msg(inSize, inData);

    while (msg.remain()) {
        Node* node = Msg_GetNode(inWorld, msg);
        if (!node)
            return kSCErr_NodeNotFound;

        int32 run = msg.geti();

        Node_SetRun(node, run);
    }

    return kSCErr_None;
}

SCErr meth_n_map(World* inWorld, int inSize, char* inData, ReplyAddress* inReply);
SCErr meth_n_map(World* inWorld, int inSize, char* inData, ReplyAddress* /*inReply*/) {
    sc_msg_iter msg(inSize, inData);
    Node* node = Msg_GetNode(inWorld, msg);
    if (!node)
        return kSCErr_NodeNotFound;

    while (msg.remain() >= 8) {
        if (msg.nextTag('i') == 's') {
            int32* name = msg.gets4();
            int bus = msg.geti();
            Node_MapControl(node, Hash(name), name, 0, bus);
        } else {
            int32 index = msg.geti();
            int32 bus = msg.geti();
            Node_MapControl(node, index, bus);
        }
    }
    return kSCErr_None;
}

SCErr meth_n_mapn(World* inWorld, int inSize, char* inData, ReplyAddress* inReply);
SCErr meth_n_mapn(World* inWorld, int inSize, char* inData, ReplyAddress* /*inReply*/) {
    sc_msg_iter msg(inSize, inData);
    Node* node = Msg_GetNode(inWorld, msg);
    if (!node)
        return kSCErr_NodeNotFound;

    while (msg.remain() >= 12) {
        if (msg.nextTag('i') == 's') {
            int32* name = msg.gets4();
            int32 hash = Hash(name);
            int bus = msg.geti();
            int n = msg.geti();
            for (int i = 0; i < n; ++i) {
                Node_MapControl(node, hash, name, i, bus == -1 ? -1 : bus + i);
            }
        } else {
            int32 index = msg.geti();
            int32 bus = msg.geti();
            int n = msg.geti();
            for (int i = 0; i < n; ++i) {
                Node_MapControl(node, index + i, bus == -1 ? -1 : bus + i);
            }
        }
    }
    return kSCErr_None;
}

SCErr meth_n_mapa(World* inWorld, int inSize, char* inData, ReplyAddress* inReply);
SCErr meth_n_mapa(World* inWorld, int inSize, char* inData, ReplyAddress* /*inReply*/) {
    sc_msg_iter msg(inSize, inData);
    Node* node = Msg_GetNode(inWorld, msg);
    if (!node)
        return kSCErr_NodeNotFound;

    while (msg.remain() >= 8) {
        if (msg.nextTag('i') == 's') {
            int32* name = msg.gets4();
            int bus = msg.geti();
            Node_MapAudioControl(node, Hash(name), name, 0, bus);
        } else {
            int32 index = msg.geti();
            int32 bus = msg.geti();
            Node_MapAudioControl(node, index, bus);
        }
    }
    return kSCErr_None;
}

SCErr meth_n_mapan(World* inWorld, int inSize, char* inData, ReplyAddress* inReply);
SCErr meth_n_mapan(World* inWorld, int inSize, char* inData, ReplyAddress* /*inReply*/) {
    sc_msg_iter msg(inSize, inData);
    Node* node = Msg_GetNode(inWorld, msg);
    if (!node)
        return kSCErr_NodeNotFound;

    while (msg.remain() >= 12) {
        if (msg.nextTag('i') == 's') {
            int32* name = msg.gets4();
            int32 hash = Hash(name);
            int bus = msg.geti();
            int n = msg.geti();
            for (int i = 0; i < n; ++i) {
                Node_MapAudioControl(node, hash, name, i, bus == -1 ? -1 : bus + i);
            }
        } else {
            int32 index = msg.geti();
            int32 bus = msg.geti();
            int n = msg.geti();
            for (int i = 0; i < n; ++i) {
                Node_MapAudioControl(node, index + i, bus == -1 ? -1 : bus + i);
            }
        }
    }
    return kSCErr_None;
}

SCErr meth_n_set(World* inWorld, int inSize, char* inData, ReplyAddress* inReply);
SCErr meth_n_set(World* inWorld, int inSize, char* inData, ReplyAddress* /*inReply*/) {
    sc_msg_iter msg(inSize, inData);
    Node* node = Msg_GetNode(inWorld, msg);
    if (!node)
        return kSCErr_NodeNotFound;
    while (msg.remain() >= 8) {
        int i = 0;
        int loop = 0;
        if (msg.nextTag('i') == 's') {
            int32* name = msg.gets4();
            int32 hash = Hash(name);
            do {
                switch (msg.nextTag('f')) {
                case 'f':
                case 'i': {
                    float32 value = msg.getf();
                    Node_SetControl(node, hash, name, i, value);
                    ++i;
                    break;
                }
                case 's': {
                    const char* string = msg.gets();
                    if (*string == 'c') {
                        int bus = sc_atoi(string + 1);
                        Node_MapControl(node, hash, name, i, bus);
                        ++i;
                    }
                    if (*string == 'a') {
                        int bus = sc_atoi(string + 1);
                        Node_MapAudioControl(node, hash, name, i, bus);
                        ++i;
                    }
                    break;
                }
                case ']':
                    msg.count++;
                    loop--;
                    break;
                case '[':
                    msg.count++;
                    loop++;
                    break;
                }
            } while (loop);
        } else {
            int32 index = msg.geti();
            do {
                switch (msg.nextTag('f')) {
                case 'f':
                case 'i': {
                    float32 value = msg.getf();
                    Node_SetControl(node, index + i, value);
                    ++i;
                    break;
                }
                case 's': {
                    const char* string = msg.gets();
                    if (*string == 'c') {
                        int bus = sc_atoi(string + 1);
                        Node_MapControl(node, index + i, bus);
                        ++i;
                    }
                    if (*string == 'a') {
                        int bus = sc_atoi(string + 1);
                        Node_MapAudioControl(node, index + i, bus);
                        ++i;
                    }
                    break;
                }
                case ']':
                    msg.count++;
                    loop--;
                    break;
                case '[':
                    msg.count++;
                    loop++;
                    break;
                }
            } while (loop);
        }
    }

    //{
    //	int i = 0;
    //	int loop = 0;
    //	if (msg.nextTag('i') == 's') {
    //	    int32* name = msg.gets4();
    //	    int32 hash = Hash(name);
    //	    if (msg.nextTag('f') == '[' ) {
    //		msg.count++;
    //		loop = 1;
    //	    }
    //	    do {
    //		if (msg.nextTag('f') == 's' ) {
    //		    const char* string = msg.gets();
    //		    if ( *string == 'c') {
    //			int bus = sc_atoi(string+1);
    //			Node_MapControl(node, hash, name, i, bus);
    //		    }
    //		} else {
    //		    if (msg.nextTag('f') == ']' ) {
    //			msg.count++;
    //			loop = 0;
    //		    } else {
    //			float32 value = msg.getf();
    //			Node_SetControl(node, hash, name, i, value);
    //		    }
    //		}
    //		++i;
    //	    } while (loop);
    //	} else {
    //	    int32 index = msg.geti();
    //	    if (msg.nextTag('f') == '[' ) {
    //		msg.count++;
    //		loop = 1;
    //	    }
    //	    do {
    //		if (msg.nextTag('f') == 's') {
    //		    const char* string = msg.gets();
    //		    if (*string == 'c') {
    //			int bus = sc_atoi(string+1);
    //			Node_MapControl(node, index + i, bus);
    //		    }
    //		} else {
    //		    if (msg.nextTag('f') == ']' ) {
    //			msg.count++;
    //			loop = 0;
    //		    } else {
    //			float32 value = msg.getf();
    //			Node_SetControl(node, index + i, value);
    //		    }
    //		}
    //		++i;
    //	    } while (loop);
    //	}
    //    }
    return kSCErr_None;
}

SCErr meth_n_setn(World* inWorld, int inSize, char* inData, ReplyAddress* inReply);
SCErr meth_n_setn(World* inWorld, int inSize, char* inData, ReplyAddress* /*inReply*/) {
    sc_msg_iter msg(inSize, inData);
    Node* node = Msg_GetNode(inWorld, msg);
    if (!node)
        return kSCErr_NodeNotFound;

    while (msg.remain()) {
        if (msg.nextTag('i') == 's') {
            int32* name = msg.gets4();
            int32 hash = Hash(name);
            int32 n = msg.geti();
            for (int i = 0; msg.remain() && i < n; ++i) {
                if (msg.nextTag('f') == 's') {
                    const char* string = msg.gets();
                    if (*string == 'c') {
                        int bus = sc_atoi(string + 1);
                        Node_MapControl(node, hash, name, i, bus);
                    }
                    if (*string == 'a') {
                        int bus = sc_atoi(string + 1);
                        Node_MapAudioControl(node, hash, name, i, bus);
                    }

                } else {
                    float32 value = msg.getf();
                    Node_SetControl(node, hash, name, i, value);
                }
            }
        } else {
            int32 index = msg.geti();
            int32 n = msg.geti();
            for (int i = 0; msg.remain() && i < n; ++i) {
                if (msg.nextTag('f') == 's') {
                    const char* string = msg.gets();
                    if (*string == 'c') {
                        int bus = sc_atoi(string + 1);
                        Node_MapControl(node, index + i, bus);
                    }
                    if (*string == 'a') {
                        int bus = sc_atoi(string + 1);
                        Node_MapAudioControl(node, index + i, bus);
                    }
                } else {
                    float32 value = msg.getf();
                    Node_SetControl(node, index + i, value);
                }
            }
        }
    }
    return kSCErr_None;
}

SCErr meth_n_fill(World* inWorld, int inSize, char* inData, ReplyAddress* inReply);
SCErr meth_n_fill(World* inWorld, int inSize, char* inData, ReplyAddress* /*inReply*/) {
    sc_msg_iter msg(inSize, inData);
    Node* node = Msg_GetNode(inWorld, msg);
    if (!node)
        return kSCErr_NodeNotFound;

    while (msg.remain() >= 12) {
        if (msg.nextTag('i') == 's') {
            int32* name = msg.gets4();
            int32 hash = Hash(name);
            int32 n = msg.geti();
            float32 value = msg.getf();

            for (int i = 0; i < n; ++i) {
                Node_SetControl(node, hash, name, i, value);
            }

            // =============================================================================
            // TAU BUG FIX: Removed erroneous else block from upstream
            // The upstream code had an else block that called msg.getf() to read another
            // value after the bus mapping check. But per the OSC spec, /n_fill only takes
            // [controlIndex, numControls, value] - there's no second value to read.
            // The else block would read garbage/undefined data and overwrite the correct
            // values that were just set. This was likely a copy-paste error from /n_set
            // which has a similar bus-mapping else pattern.
            // Test: test/osc_semantic.spec.mjs "/n_fill semantic tests"
            // =============================================================================
            if (msg.nextTag('f') == 's') {
                const char* string = msg.gets();
                if (*string == 'c') {
                    int bus = sc_atoi(string + 1);
                    for (int i = 0; i < n; ++i) {
                        Node_MapControl(node, hash, name, i, bus + i);
                    }
                }
            }
            // No else block - if no bus mapping string, the value was already set above
        } else {
            int32 index = msg.geti();
            int32 n = msg.geti();
            float32 value = msg.getf();

            for (int i = 0; i < n; ++i) {
                Node_SetControl(node, index + i, value);
            }
            // =============================================================================
            // TAU BUG FIX: Same fix as above for index-based path
            // =============================================================================
            if (msg.nextTag('f') == 's') {
                const char* string = msg.gets();
                if (*string == 'c') {
                    int bus = sc_atoi(string + 1);
                    for (int i = 0; i < n; ++i) {
                        Node_MapControl(node, index + i, bus + i);
                    }
                }
            }
            // No else block - if no bus mapping string, the value was already set above
        }
    }

    return kSCErr_None;
}


SCErr meth_n_query(World* inWorld, int inSize, char* inData, ReplyAddress* inReply);
SCErr meth_n_query(World* inWorld, int inSize, char* inData, ReplyAddress* inReply) {
    sc_msg_iter msg(inSize, inData);

    while (msg.remain()) {
        Node* node = Msg_GetNode(inWorld, msg);
        if (!node)
            return kSCErr_NodeNotFound;

        Node_StateMsg(node, kNode_Info);
    }
    return kSCErr_None;
}

SCErr meth_b_query(World* inWorld, int inSize, char* inData, ReplyAddress* inReply);
SCErr meth_b_query(World* inWorld, int inSize, char* inData, ReplyAddress* inReply) {
    sc_msg_iter msg(inSize, inData);

    small_scpacket packet;

    int numbufs = msg.remain() >> 2;
    packet.adds("/b_info");
    packet.maketags(numbufs * 4 + 1);
    packet.addtag(',');

    while (msg.remain()) {
        int bufindex = msg.geti();
        SndBuf* buf = World_GetBuf(inWorld, bufindex);

        packet.addtag('i');
        packet.addtag('i');
        packet.addtag('i');
        packet.addtag('f');
        packet.addi(bufindex);
        packet.addi(buf->frames);
        packet.addi(buf->channels);
        packet.addf(buf->samplerate);
    }

    if (packet.size()) {
        CallSequencedCommand(SendReplyCmd, inWorld, packet.size(), packet.data(), inReply);
    }

    return kSCErr_None;
}


#ifndef SC_LEAN_TARGET
SCErr meth_d_load(World* inWorld, int inSize, char* inData, ReplyAddress* inReply);
SCErr meth_d_load(World* inWorld, int inSize, char* inData, ReplyAddress* inReply) {
    CallSequencedCommand(LoadSynthDefCmd, inWorld, inSize, inData, inReply);

    return kSCErr_None;
}
#endif // !SC_LEAN_TARGET

SCErr meth_d_recv(World* inWorld, int inSize, char* inData, ReplyAddress* inReply);
SCErr meth_d_recv(World* inWorld, int inSize, char* inData, ReplyAddress* inReply) {
    CallSequencedCommand(RecvSynthDefCmd, inWorld, inSize, inData, inReply);

    return kSCErr_None;
}

#ifndef SC_LEAN_TARGET
SCErr meth_d_loadDir(World* inWorld, int inSize, char* inData, ReplyAddress* inReply);
SCErr meth_d_loadDir(World* inWorld, int inSize, char* inData, ReplyAddress* inReply) {
    CallSequencedCommand(LoadSynthDefDirCmd, inWorld, inSize, inData, inReply);

    return kSCErr_None;
}
#endif // !SC_LEAN_TARGET

SCErr meth_d_freeAll(World* inWorld, int inSize, char* inData, ReplyAddress* inReply);
SCErr meth_d_freeAll(World* inWorld, int /*inSize*/, char* /*inData*/, ReplyAddress* /*inReply*/) {
    World_FreeAllGraphDefs(inWorld);
    return kSCErr_None;
}

SCErr meth_d_free(World* inWorld, int inSize, char* inData, ReplyAddress* inReply);
SCErr meth_d_free(World* inWorld, int inSize, char* inData, ReplyAddress* inReply) {
    sc_msg_iter msg(inSize, inData);
    while (msg.remain()) {
        int32* defname = msg.gets4();
        if (!defname)
            return kSCErr_SynthDefNotFound;
        SCErr err = GraphDef_Remove(inWorld, defname);
        if (err != kSCErr_None)
            return err;
    }
    return kSCErr_None;
}


// [SuperSonic] Declared in SC_Graph.cpp. Runs UGen constructors + zombie check
// synchronously so /n_set (and siblings) later in the same bundle mutate the
// post-init graph state instead of racing the constructors.
bool Graph_InitUnits(Graph* inGraph);

// 'argtype' is false for setn type args
SCErr meth_s_do_new(World* inWorld, int inSize, char* inData, bool argtype) {
    SCErr err;
    sc_msg_iter msg(inSize, inData);
    int32* defname = msg.gets4();
    if (!defname)
        return kSCErr_WrongArgType;

    int32 nodeID = msg.geti();
    int32 addAction = msg.geti();

    GraphDef* def = World_GetGraphDef(inWorld, defname);
    if (!def) {
        clockwork_log("*** ERROR: SynthDef %s not found\n", (char*)defname);
        return kSCErr_SynthDefNotFound;
    }

    Graph* graph = nullptr;
    switch (addAction) {
    case 0: {
        Group* group = Msg_GetGroup(inWorld, msg);
        if (!group)
            return kSCErr_GroupNotFound;
        err = Graph_New(inWorld, def, nodeID, &msg, &graph, argtype);
        if (err)
            return err;
        if (!graph)
            return kSCErr_Failed;
        Group_AddHead(group, &graph->mNode);
    } break;
    case 1: {
        Group* group = Msg_GetGroup(inWorld, msg);
        if (!group)
            return kSCErr_GroupNotFound;
        err = Graph_New(inWorld, def, nodeID, &msg, &graph, argtype);
        if (err)
            return err;
        Group_AddTail(group, &graph->mNode);
    } break;
    case 2: {
        Node* beforeThisNode = Msg_GetNode(inWorld, msg);
        if (!beforeThisNode)
            return kSCErr_NodeNotFound;
        err = Graph_New(inWorld, def, nodeID, &msg, &graph, argtype);
        if (err)
            return err;
        Node_AddBefore(&graph->mNode, beforeThisNode);
    } break;
    case 3: {
        Node* afterThisNode = Msg_GetNode(inWorld, msg);
        if (!afterThisNode)
            return kSCErr_NodeNotFound;
        err = Graph_New(inWorld, def, nodeID, &msg, &graph, argtype);
        if (err)
            return err;
        Node_AddAfter(&graph->mNode, afterThisNode);
    } break;
    case 4: {
        Node* replaceThisNode = Msg_GetNode(inWorld, msg);
        if (!replaceThisNode)
            return kSCErr_NodeNotFound;
        err = Graph_New(inWorld, def, nodeID, &msg, &graph, argtype);
        if (err)
            return err;
        Node_Replace(&graph->mNode, replaceThisNode);
    } break;
    default:
        return kSCErr_Failed;
    }
    Node_StateMsg(&graph->mNode, kNode_Go);
    // [SuperSonic] Eager UGen construction — makes intra-bundle /n_set after
    // /s_new target the post-init state (fixes slides that would otherwise
    // see the /n_set value clobber the /s_new's initial args before VarLag
    // et al. get to capture them in their Ctors).
    Graph_InitUnits(graph);
    return kSCErr_None;
}

SCErr meth_s_new(World* inWorld, int inSize, char* inData, ReplyAddress* inReply);
SCErr meth_s_new(World* inWorld, int inSize, char* inData, ReplyAddress*) {
    return meth_s_do_new(inWorld, inSize, inData, true);
}

SCErr meth_s_newargs(World* inWorld, int inSize, char* inData, ReplyAddress* inReply);
SCErr meth_s_newargs(World* inWorld, int inSize, char* inData, ReplyAddress*) {
    return meth_s_do_new(inWorld, inSize, inData, false);
}

SCErr meth_g_new(World* inWorld, int inSize, char* inData, ReplyAddress* inReply);
SCErr meth_g_new(World* inWorld, int inSize, char* inData, ReplyAddress* /*inReply*/) {
    SCErr err;

    sc_msg_iter msg(inSize, inData);
    while (msg.remain()) {
        int32 newGroupID = msg.geti();
        int32 addAction = msg.geti();

        Group* newGroup = nullptr;
        switch (addAction) {
        case 0: {
            Group* group = Msg_GetGroup(inWorld, msg);
            if (!group)
                return kSCErr_GroupNotFound;
            err = Group_New(inWorld, newGroupID, &newGroup);
            if (err) {
                if (err == kSCErr_DuplicateNodeID) {
                    newGroup = World_GetGroup(inWorld, newGroupID);
                    if (!newGroup || !newGroup->mNode.mParent || newGroup->mNode.mParent != group)
                        return err;
                } else
                    return err;
            } else {
                Group_AddHead(group, &newGroup->mNode);
            }
        } break;
        case 1: {
            Group* group = Msg_GetGroup(inWorld, msg);
            if (!group)
                return kSCErr_GroupNotFound;
            err = Group_New(inWorld, newGroupID, &newGroup);
            if (err) {
                if (err == kSCErr_DuplicateNodeID) {
                    newGroup = World_GetGroup(inWorld, newGroupID);
                    if (!newGroup || !newGroup->mNode.mParent || newGroup->mNode.mParent != group)
                        return err;
                } else
                    return err;
            } else {
                Group_AddTail(group, &newGroup->mNode);
            }
        } break;
        case 2: {
            Node* beforeThisNode = Msg_GetNode(inWorld, msg);
            if (!beforeThisNode)
                return kSCErr_TargetNodeNotFound;
            err = Group_New(inWorld, newGroupID, &newGroup);
            if (err) {
                if (err == kSCErr_DuplicateNodeID) {
                    newGroup = World_GetGroup(inWorld, newGroupID);
                    if (!newGroup || !newGroup->mNode.mParent
                        || newGroup->mNode.mParent->mNode.mID != beforeThisNode->mParent->mNode.mID)
                        return err;
                } else
                    return err;
            } else {
                Node_AddBefore(&newGroup->mNode, beforeThisNode);
            }
        } break;
        case 3: {
            Node* afterThisNode = Msg_GetNode(inWorld, msg);
            if (!afterThisNode)
                return kSCErr_TargetNodeNotFound;
            err = Group_New(inWorld, newGroupID, &newGroup);
            if (err) {
                if (err == kSCErr_DuplicateNodeID) {
                    newGroup = World_GetGroup(inWorld, newGroupID);
                    if (!newGroup || !newGroup->mNode.mParent
                        || newGroup->mNode.mParent->mNode.mID != afterThisNode->mParent->mNode.mID)
                        return err;
                } else
                    return err;
            } else {
                Node_AddAfter(&newGroup->mNode, afterThisNode);
            }
        } break;
        case 4: {
            Node* replaceThisNode = Msg_GetNode(inWorld, msg);
            if (!replaceThisNode)
                return kSCErr_TargetNodeNotFound;
            if (replaceThisNode->mID == 0)
                return kSCErr_ReplaceRootGroup;
            Node_RemoveID(replaceThisNode);

            err = Group_New(inWorld, newGroupID, &newGroup);
            if (err)
                return err;
            Node_Replace(&newGroup->mNode, replaceThisNode);
        } break;
        default:
            return kSCErr_Failed;
        }

        Node_StateMsg(&newGroup->mNode, kNode_Go);
    }

    return kSCErr_None;
}

SCErr meth_p_new(World* inWorld, int inSize, char* inData, ReplyAddress* inReply);
SCErr meth_p_new(World* inWorld, int inSize, char* inData, ReplyAddress* inReply) {
    /* we emulate the concept of parallel groups by using sequential groups */
    return meth_g_new(inWorld, inSize, inData, inReply);
}

SCErr meth_n_free(World* inWorld, int inSize, char* inData, ReplyAddress* inReply);
SCErr meth_n_free(World* inWorld, int inSize, char* inData, ReplyAddress* /*inReply*/) {
    sc_msg_iter msg(inSize, inData);

    while (msg.remain()) {
        Node* node = Msg_GetNode(inWorld, msg);
        if (!node)
            return kSCErr_NodeNotFound;

        Node_Delete(node);
    }

    return kSCErr_None;
}

SCErr meth_n_before(World* inWorld, int inSize, char* inData, ReplyAddress* inReply);
SCErr meth_n_before(World* inWorld, int inSize, char* inData, ReplyAddress* /*inReply*/) {
    sc_msg_iter msg(inSize, inData);

    Node* prevNode = nullptr;
    Node* prevTarget = nullptr;
    while (msg.remain()) {
        Node* node = Msg_GetNode(inWorld, msg);
        Node* target = Msg_GetNode(inWorld, msg);

        if (!node || !target)
            continue; // tolerate failure

        if (prevNode && prevNode != node) {
            // move the last pair that succeeded
            Node_Remove(prevNode);
            Node_AddBefore(prevNode, prevTarget);
            Node_StateMsg(prevNode, kNode_Move);
        }

        prevNode = node;
        prevTarget = target;
    }
    if (prevNode && prevNode != prevTarget) {
        // move the last pair that succeeded
        Node_Remove(prevNode);
        Node_AddBefore(prevNode, prevTarget);
        Node_StateMsg(prevNode, kNode_Move);
    }

    return kSCErr_None;
}

SCErr meth_n_after(World* inWorld, int inSize, char* inData, ReplyAddress* inReply);
SCErr meth_n_after(World* inWorld, int inSize, char* inData, ReplyAddress* /*inReply*/) {
    sc_msg_iter msg(inSize, inData);

    Node* prevNode = nullptr;
    Node* prevTarget = nullptr;
    while (msg.remain()) {
        Node* node = Msg_GetNode(inWorld, msg);
        Node* target = Msg_GetNode(inWorld, msg);

        if (!node || !target)
            continue; // tolerate failure

        if (prevNode && prevNode != node) {
            // move the last pair that succeeded
            Node_Remove(prevNode);
            Node_AddAfter(prevNode, prevTarget);
            Node_StateMsg(prevNode, kNode_Move);
        }

        prevNode = node;
        prevTarget = target;
    }
    if (prevNode) {
        // move the last pair that succeeded
        Node_Remove(prevNode);
        Node_AddAfter(prevNode, prevTarget);
        Node_StateMsg(prevNode, kNode_Move);
    }

    return kSCErr_None;
}

SCErr meth_n_order(World* inWorld, int inSize, char* inData, ReplyAddress* inReply);
SCErr meth_n_order(World* inWorld, int inSize, char* inData, ReplyAddress* /*inReply*/) {
    Node* prevNode = nullptr;
    Node* node = nullptr;

    sc_msg_iter msg(inSize, inData);
    int32 addAction = msg.geti();

    // place the first node in the list based on target and addAction
    switch (addAction) {
    case 0: {
        Group* group = Msg_GetGroup(inWorld, msg);
        if (!group)
            return kSCErr_GroupNotFound;
        while (!node && msg.remain()) {
            node = Msg_GetNode(inWorld, msg);
        }
        if (!node)
            return kSCErr_NodeNotFound;

        Group* prevGroup = node->mParent;

        Node_Remove(node);

        Group_AddHead(group, node);

        if (group != prevGroup) {
            Node_StateMsg(node, kNode_Move);
        } else {
            // TAU: Update SAB tree for same-group reorder (no OSC notification)
            NodeTree_UpdateDirect(node);
        }

        prevNode = node;

    } break;
    case 1: {
        Group* group = Msg_GetGroup(inWorld, msg);
        if (!group)
            return kSCErr_GroupNotFound;
        while (!node && msg.remain()) {
            node = Msg_GetNode(inWorld, msg);
        }
        if (!node)
            return kSCErr_NodeNotFound;

        Group* prevGroup = node->mParent;

        Node_Remove(node);

        Group_AddTail(group, node);

        if (group != prevGroup) {
            Node_StateMsg(node, kNode_Move);
        } else {
            // TAU: Update SAB tree for same-group reorder (no OSC notification)
            NodeTree_UpdateDirect(node);
        }

        prevNode = node;

    } break;
    case 2: {
        Node* beforeNode = Msg_GetNode(inWorld, msg);
        if (!beforeNode)
            return kSCErr_TargetNodeNotFound;
        while (!node && msg.remain()) {
            node = Msg_GetNode(inWorld, msg);
        }
        if (!node)
            return kSCErr_NodeNotFound;


        Node_Remove(node);
        Node_AddBefore(node, beforeNode);
        Node_StateMsg(node, kNode_Move);

        prevNode = node;
    } break;
    case 3: {
        Node* afterNode = Msg_GetNode(inWorld, msg);
        if (!afterNode)
            return kSCErr_TargetNodeNotFound;
        while (!node && msg.remain()) {
            node = Msg_GetNode(inWorld, msg);
        }
        if (!node)
            return kSCErr_NodeNotFound;


        Node_Remove(node);
        Node_AddAfter(node, afterNode);
        Node_StateMsg(node, kNode_Move);

        prevNode = node;
    } break;
    default:
        return kSCErr_Failed;
    }

    // now iterate through in order
    while (msg.remain()) {
        node = Msg_GetNode(inWorld, msg);
        if (!node)
            continue;
        Node_Remove(node);
        Node_AddAfter(node, prevNode);
        Node_StateMsg(node, kNode_Move);

        prevNode = node;
    }

    return kSCErr_None;
}

SCErr meth_g_head(World* inWorld, int inSize, char* inData, ReplyAddress* inReply);
SCErr meth_g_head(World* inWorld, int inSize, char* inData, ReplyAddress* /*inReply*/) {
    sc_msg_iter msg(inSize, inData);
    while (msg.remain()) {
        Group* group = Msg_GetGroup(inWorld, msg);
        if (!group)
            return kSCErr_GroupNotFound;

        Node* node = Msg_GetNode(inWorld, msg);
        if (!node)
            return kSCErr_NodeNotFound;

        Node_Remove(node);
        Group_AddHead(group, node);

        Node_StateMsg(node, kNode_Move);
    }
    return kSCErr_None;
}

SCErr meth_g_tail(World* inWorld, int inSize, char* inData, ReplyAddress* inReply);
SCErr meth_g_tail(World* inWorld, int inSize, char* inData, ReplyAddress* /*inReply*/) {
    sc_msg_iter msg(inSize, inData);
    while (msg.remain()) {
        Group* group = Msg_GetGroup(inWorld, msg);
        if (!group)
            return kSCErr_GroupNotFound;

        Node* node = Msg_GetNode(inWorld, msg);
        if (!node)
            return kSCErr_NodeNotFound;

        Node_Remove(node);
        Group_AddTail(group, node);

        Node_StateMsg(node, kNode_Move);
    }
    return kSCErr_None;
}

SCErr meth_g_insert(World* inWorld, int inSize, char* inData, ReplyAddress* inReply);
SCErr meth_g_insert(World* inWorld, int inSize, char* inData, ReplyAddress* /*inReply*/) {
    sc_msg_iter msg(inSize, inData);
    while (msg.remain()) {
        Group* group = Msg_GetGroup(inWorld, msg);
        if (!group)
            return kSCErr_GroupNotFound;

        Node* node = Msg_GetNode(inWorld, msg);
        if (!node)
            return kSCErr_NodeNotFound;

        Group* prevGroup = node->mParent;

        int index = msg.geti();

        Node_Remove(node);
        Group_Insert(group, node, index);

        if (group != prevGroup) {
            Node_StateMsg(node, kNode_Move);
        }
    }
    return kSCErr_None;
}

SCErr meth_g_freeAll(World* inWorld, int inSize, char* inData, ReplyAddress* inReply);
SCErr meth_g_freeAll(World* inWorld, int inSize, char* inData, ReplyAddress* /*inReply*/) {
    sc_msg_iter msg(inSize, inData);
    while (msg.remain()) {
        Group* group = Msg_GetGroup(inWorld, msg);
        if (!group)
            return kSCErr_GroupNotFound;

        Group_DeleteAll(group);
    }
    return kSCErr_None;
}

SCErr meth_g_deepFree(World* inWorld, int inSize, char* inData, ReplyAddress* inReply);
SCErr meth_g_deepFree(World* inWorld, int inSize, char* inData, ReplyAddress* /*inReply*/) {
    sc_msg_iter msg(inSize, inData);
    while (msg.remain()) {
        Group* group = Msg_GetGroup(inWorld, msg);
        if (!group)
            return kSCErr_GroupNotFound;

        Group_DeepFreeGraphs(group);
    }
    return kSCErr_None;
}


SCErr meth_status(World* inWorld, int inSize, char* inData, ReplyAddress* inReply);
SCErr meth_status(World* inWorld, int inSize, char* inData, ReplyAddress* inReply) {
    CallSequencedCommand(AudioStatusCmd, inWorld, inSize, inData, inReply);
    return kSCErr_None;
}

SCErr meth_rtMemoryStatus(World* inWorld, int inSize, char* inData, ReplyAddress* inReply);
SCErr meth_rtMemoryStatus(World* inWorld, int inSize, char* inData, ReplyAddress* inReply) {
    CallSequencedCommand(RTMemStatusCmd, inWorld, inSize, inData, inReply);
    return kSCErr_None;
}

SCErr meth_quit(World* inWorld, int inSize, char* inData, ReplyAddress* inReply);
SCErr meth_quit(World* inWorld, int inSize, char* inData, ReplyAddress* inReply) {
#ifdef SC_LEAN_TARGET
    // /quit is not supported in SuperSonic - use destroy() instead
    SendFailure(inReply, "/quit", "not supported in SuperSonic - use destroy() instead");
    return kSCErr_Failed;
#else
    CallSequencedCommand(AudioQuitCmd, inWorld, inSize, inData, inReply);
    return kSCErr_None;
#endif
}

SCErr meth_clearSched(World* inWorld, int inSize, char* inData, ReplyAddress* inReply);
SCErr meth_clearSched(World* inWorld, int inSize, char* inData, ReplyAddress* inReply) {
    if (inWorld->mRealTime) {
        inWorld->hw->mAudioDriver->ClearSched();
    }
    return kSCErr_None;
}

SCErr meth_dumpOSC(World* inWorld, int inSize, char* inData, ReplyAddress* inReply);
SCErr meth_dumpOSC(World* inWorld, int inSize, char* inData, ReplyAddress* inReply) {
    sc_msg_iter msg(inSize, inData);
    inWorld->mDumpOSC = msg.geti();
    return kSCErr_None;
}


SCErr meth_version(World* inWorld, int inSize, char* inData, ReplyAddress* inReply);
SCErr meth_version(World* inWorld, int inSize, char* inData, ReplyAddress* inReply) {
    sc_msg_iter msg(inSize, inData);

    small_scpacket packet;
    packet.adds("/version.reply");
    packet.maketags(7);
    packet.addtag(',');
    packet.addtag('s');
    packet.adds("scsynth");
    packet.addtag('i');
    packet.addi(SC_VersionMajor);
    packet.addtag('i');
    packet.addi(SC_VersionMinor);
    packet.addtag('s');
    packet.adds(SC_VersionPostfix);
    packet.addtag('s');
    packet.adds(SC_BranchOrTag);
    packet.addtag('s');
    packet.adds(SC_CommitHash);

    CallSequencedCommand(SendReplyCmd, inWorld, packet.size(), packet.data(), inReply);

    return kSCErr_None;
}

SCErr meth_b_set(World* inWorld, int inSize, char* inData, ReplyAddress* inReply);
SCErr meth_b_set(World* inWorld, int inSize, char* inData, ReplyAddress* /*inReply*/) {
    sc_msg_iter msg(inSize, inData);
    int bufindex = msg.geti();
    SndBuf* buf = World_GetBuf(inWorld, bufindex);
    if (!buf)
        return kSCErr_Failed;

    float* data = buf->data;
    uint32 numSamples = buf->samples;

    while (msg.remain() >= 8) {
        uint32 sampleIndex = msg.geti();
        float32 value = msg.getf();
        if (sampleIndex < numSamples) {
            data[sampleIndex] = value;
        } else
            return kSCErr_IndexOutOfRange;
    }
    return kSCErr_None;
}

SCErr meth_b_setn(World* inWorld, int inSize, char* inData, ReplyAddress* inReply);
SCErr meth_b_setn(World* inWorld, int inSize, char* inData, ReplyAddress* /*inReply*/) {
    sc_msg_iter msg(inSize, inData);
    int bufindex = msg.geti();
    SndBuf* buf = World_GetBuf(inWorld, bufindex);
    if (!buf)
        return kSCErr_Failed;

    float* data = buf->data;
    int numSamples = buf->samples;

    while (msg.remain()) {
        int32 start = msg.geti();
        int32 n = msg.geti();
        int32 end = start + n - 1;

        if (end < 0 || start >= numSamples)
            continue;

        start = sc_clip(start, 0, numSamples - 1);
        end = sc_clip(end, 0, numSamples - 1);

        for (int i = start; msg.remain() && i <= end; ++i) {
            float32 value = msg.getf();
            data[i] = value;
        }
    }

    return kSCErr_None;
}


SCErr meth_b_fill(World* inWorld, int inSize, char* inData, ReplyAddress* inReply);
SCErr meth_b_fill(World* inWorld, int inSize, char* inData, ReplyAddress* /*inReply*/) {
    sc_msg_iter msg(inSize, inData);
    int bufindex = msg.geti();
    SndBuf* buf = World_GetBuf(inWorld, bufindex);
    if (!buf)
        return kSCErr_Failed;

    float* data = buf->data;
    int numSamples = buf->samples;

    while (msg.remain() >= 12) {
        int32 start = msg.geti();
        int32 n = msg.geti();
        float32 value = msg.getf();
        int32 end = start + n - 1;

        if (end < 0 || start >= numSamples)
            continue;

        start = sc_clip(start, 0, numSamples - 1);
        end = sc_clip(end, 0, numSamples - 1);

        for (int i = start; i <= end; ++i)
            data[i] = value;
    }

    return kSCErr_None;
}

SCErr meth_b_gen(World* inWorld, int inSize, char* inData, ReplyAddress* inReply);
SCErr meth_b_gen(World* inWorld, int inSize, char* inData, ReplyAddress* inReply) {
    CallSequencedCommand(BufGenCmd, inWorld, inSize, inData, inReply);

    return kSCErr_None;
}


SCErr meth_c_set(World* inWorld, int inSize, char* inData, ReplyAddress* inReply);
SCErr meth_c_set(World* inWorld, int inSize, char* inData, ReplyAddress* /*inReply*/) {
    sc_msg_iter msg(inSize, inData);

    float* data = inWorld->mControlBus;
    int32* touched = inWorld->mControlBusTouched;
    int32 bufCounter = inWorld->mBufCounter;
    uint32 maxIndex = inWorld->mNumControlBusChannels;

    while (msg.remain() >= 8) {
        uint32 index = msg.geti();
        float32 value = msg.getf();
        if (index < maxIndex) {
            data[index] = value;
            touched[index] = bufCounter;
        } else
            return kSCErr_IndexOutOfRange;
    }
    return kSCErr_None;
}

SCErr meth_c_setn(World* inWorld, int inSize, char* inData, ReplyAddress* inReply);
SCErr meth_c_setn(World* inWorld, int inSize, char* inData, ReplyAddress* /*inReply*/) {
    sc_msg_iter msg(inSize, inData);

    float* data = inWorld->mControlBus;
    int32* touched = inWorld->mControlBusTouched;
    int32 bufCounter = inWorld->mBufCounter;
    int maxIndex = inWorld->mNumControlBusChannels;

    while (msg.remain()) {
        int32 start = msg.geti();
        int32 n = msg.geti();
        int32 end = start + n - 1;

        if (start < 0 || end >= maxIndex || start > end)
            return kSCErr_IndexOutOfRange;

        for (int i = start; msg.remain() && i <= end; ++i) {
            float32 value = msg.getf();
            data[i] = value;
            touched[i] = bufCounter;
        }
    }

    return kSCErr_None;
}


SCErr meth_c_get(World* inWorld, int inSize, char* inData, ReplyAddress* inReply);
SCErr meth_c_get(World* inWorld, int inSize, char* inData, ReplyAddress* inReply) {
    sc_msg_iter msg(inSize, inData);

    float* data = inWorld->mControlBus;
    uint32 maxIndex = inWorld->mNumControlBusChannels;

    int numheads = msg.remain() >> 2;

    big_scpacket packet;
    packet.adds("/c_set");
    packet.maketags(numheads * 2 + 1);
    packet.addtag(',');

    while (msg.remain() >= 4) {
        uint32 index = msg.geti();
        if (index >= maxIndex)
            return kSCErr_IndexOutOfRange;
        packet.addtag('i');
        packet.addtag('f');
        packet.addi(index);
        packet.addf(data[index]);
    }

    if (packet.size()) {
        CallSequencedCommand(SendReplyCmd, inWorld, packet.size(), packet.data(), inReply);
    }

    return kSCErr_None;
}

SCErr meth_c_getn(World* inWorld, int inSize, char* inData, ReplyAddress* inReply);
SCErr meth_c_getn(World* inWorld, int inSize, char* inData, ReplyAddress* inReply) {
    sc_msg_iter msg(inSize, inData);

    float* data = inWorld->mControlBus;
    int maxIndex = inWorld->mNumControlBusChannels;

    // figure out how many tags to allocate
    int numcontrols = 0;
    int numheads = msg.remain() >> 3;

    while (msg.remain()) {
        msg.geti(); // skip start
        int32 n = msg.geti();
        numcontrols += n;
    }

    big_scpacket packet;
    packet.adds("/c_setn");
    packet.maketags(numheads * 2 + numcontrols + 1);
    packet.addtag(',');

    // start over at beginning of message
    msg.init(inSize, inData);

    while (msg.remain()) {
        int32 start = msg.geti();
        int32 n = msg.geti();
        int32 end = start + n - 1;

        if (start < 0 || end >= maxIndex || start > end)
            return kSCErr_IndexOutOfRange;

        packet.addtag('i');
        packet.addtag('i');
        packet.addi(start);
        packet.addi(n);

        for (int i = start; i <= end; ++i) {
            packet.addtag('f');
            packet.addf(data[i]);
        }
    }

    if (packet.size()) {
        CallSequencedCommand(SendReplyCmd, inWorld, packet.size(), packet.data(), inReply);
    }

    return kSCErr_None;
}


SCErr meth_c_fill(World* inWorld, int inSize, char* inData, ReplyAddress* inReply);
SCErr meth_c_fill(World* inWorld, int inSize, char* inData, ReplyAddress* /*inReply*/) {
    sc_msg_iter msg(inSize, inData);

    float* data = inWorld->mControlBus;
    int32* touched = inWorld->mControlBusTouched;
    int32 bufCounter = inWorld->mBufCounter;
    int maxIndex = inWorld->mNumControlBusChannels;

    while (msg.remain() >= 12) {
        int32 start = msg.geti();
        int32 n = msg.geti();
        float32 value = msg.getf();
        int32 end = start + n - 1;

        if (end < 0 || start >= maxIndex)
            continue;

        start = sc_clip(start, 0, maxIndex - 1);
        end = sc_clip(end, 0, maxIndex - 1);

        for (int i = start; i <= end; ++i) {
            data[i] = value;
            touched[i] = bufCounter;
        }
    }

    return kSCErr_None;
}


SCErr meth_b_get(World* inWorld, int inSize, char* inData, ReplyAddress* inReply);
SCErr meth_b_get(World* inWorld, int inSize, char* inData, ReplyAddress* inReply) {
    sc_msg_iter msg(inSize, inData);
    int bufindex = msg.geti();
    SndBuf* buf = World_GetBuf(inWorld, bufindex);
    if (!buf)
        return kSCErr_Failed;

    float* data = buf->data;
    uint32 maxIndex = buf->samples;

    int numheads = msg.remain() >> 2;

    big_scpacket packet;
    packet.adds("/b_set");
    packet.maketags(numheads * 2 + 2);
    packet.addtag(',');
    packet.addtag('i');
    packet.addi(bufindex);

    while (msg.remain() >= 4) {
        uint32 index = msg.geti();
        if (index >= maxIndex)
            return kSCErr_IndexOutOfRange;
        packet.addtag('i');
        packet.addtag('f');
        packet.addi(index);
        packet.addf(data[index]);
    }

    if (packet.size()) {
        CallSequencedCommand(SendReplyCmd, inWorld, packet.size(), packet.data(), inReply);
    }

    return kSCErr_None;
}

SCErr meth_b_getn(World* inWorld, int inSize, char* inData, ReplyAddress* inReply);
SCErr meth_b_getn(World* inWorld, int inSize, char* inData, ReplyAddress* inReply) {
    sc_msg_iter msg(inSize, inData);
    int bufindex = msg.geti();
    SndBuf* buf = World_GetBuf(inWorld, bufindex);
    if (!buf)
        return kSCErr_Failed;

    float* data = buf->data;
    int32 maxIndex = buf->samples;

    // figure out how many tags to allocate
    int numcontrols = 0;
    int numheads = msg.remain() >> 3;

    while (msg.remain()) {
        msg.geti(); // skip start
        int32 n = msg.geti();
        numcontrols += n;
    }

    big_scpacket packet;
    packet.adds("/b_setn");
    packet.maketags(numheads * 2 + numcontrols + 2);
    packet.addtag(',');

    // start over at beginning of message
    msg.init(inSize, inData);
    msg.geti(); // skip buf index

    packet.addtag('i');
    packet.addi(bufindex);

    while (msg.remain()) {
        int32 start = msg.geti();
        int32 n = msg.geti();
        int32 end = start + n - 1;

        if (start < 0 || end >= maxIndex || start > end)
            return kSCErr_IndexOutOfRange;

        packet.addtag('i');
        packet.addtag('i');
        packet.addi(start);
        packet.addi(n);

        for (int i = start; i <= end; ++i) {
            packet.addtag('f');
            packet.addf(data[i]);
        }
    }

    if (packet.size()) {
        CallSequencedCommand(SendReplyCmd, inWorld, packet.size(), packet.data(), inReply);
    }

    return kSCErr_None;
}


SCErr meth_s_get(World* inWorld, int inSize, char* inData, ReplyAddress* inReply);
SCErr meth_s_get(World* inWorld, int inSize, char* inData, ReplyAddress* inReply) {
    sc_msg_iter msg(inSize, inData);

    Graph* graph = Msg_GetGraph(inWorld, msg);
    if (!graph)
        return kSCErr_NodeNotFound;

    int numheads = msg.tags ? strlen(msg.tags) - 1 : msg.remain() >> 2;

    big_scpacket packet;
    packet.adds("/n_set");
    packet.maketags(numheads * 2 + 2);
    packet.addtag(',');
    packet.addtag('i');
    packet.addi(graph->mNode.mID);

    while (msg.remain() >= 4) {
        if (msg.nextTag('i') == 's') {
            int32* name = msg.gets4();
            int32 hash = Hash(name);
            float32 value = 0.f;
            Graph_GetControl(graph, hash, name, 0, value);
            packet.addtag('s');
            packet.addtag('f');
            packet.adds((char*)name);
            packet.addf(value);
        } else {
            int32 index = msg.geti();
            float32 value = 0.f;
            Graph_GetControl(graph, index, value);
            packet.addtag('i');
            packet.addtag('f');
            packet.addi(index);
            packet.addf(value);
        }
    }

    if (packet.size()) {
        CallSequencedCommand(SendReplyCmd, inWorld, packet.size(), packet.data(), inReply);
    }

    return kSCErr_None;
}

SCErr meth_s_getn(World* inWorld, int inSize, char* inData, ReplyAddress* inReply);
SCErr meth_s_getn(World* inWorld, int inSize, char* inData, ReplyAddress* inReply) {
    sc_msg_iter msg(inSize, inData);

    Graph* graph = Msg_GetGraph(inWorld, msg);
    if (!graph)
        return kSCErr_NodeNotFound;

    // figure out how many tags to allocate
    int numcontrols = 0;
    int numheads = msg.tags ? (strlen(msg.tags) - 1) >> 1 : msg.remain() >> 3;

    while (msg.remain()) {
        msg.geti(); // skip start
        int32 n = msg.geti();
        numcontrols += n;
    }

    big_scpacket packet;
    packet.adds("/n_setn");
    packet.maketags(numheads * 2 + numcontrols + 2);
    packet.addtag(',');

    // start over at beginning of message
    msg.init(inSize, inData);
    msg.geti(); // skip buf index

    packet.addtag('i');
    packet.addi(graph->mNode.mID);

    while (msg.remain()) {
        if (msg.nextTag('i') == 's') {
            int32* name = msg.gets4();
            int32 hash = Hash(name);
            int32 n = msg.geti();
            packet.addtag('s');
            packet.addtag('i');
            packet.adds((char*)name);
            packet.addi(n);
            for (int i = 0; i < n; ++i) {
                float32 value = 0.f;
                Graph_GetControl(graph, hash, name, i, value);
                packet.addtag('f');
                packet.addf(value);
            }
        } else {
            int32 index = msg.geti();
            int32 n = msg.geti();
            packet.addtag('i');
            packet.addtag('i');
            packet.addi(index);
            packet.addi(n);
            for (int i = 0; i < n; ++i) {
                float32 value = 0.f;
                Graph_GetControl(graph, index + i, value);
                packet.addtag('f');
                packet.addf(value);
            }
        }
    }

    if (packet.size()) {
        CallSequencedCommand(SendReplyCmd, inWorld, packet.size(), packet.data(), inReply);
    }

    return kSCErr_None;
}


SCErr meth_s_noid(World* inWorld, int inSize, char* inData, ReplyAddress* inReply);
SCErr meth_s_noid(World* inWorld, int inSize, char* inData, ReplyAddress* inReply) {
    sc_msg_iter msg(inSize, inData);
    while (msg.remain()) {
        Graph* graph = Msg_GetGraph(inWorld, msg);
        if (!graph)
            continue;

        Node_RemoveID(&graph->mNode);
    }

    return kSCErr_None;
}


SCErr meth_notify(World* inWorld, int inSize, char* inData, ReplyAddress* inReply);
SCErr meth_notify(World* inWorld, int inSize, char* inData, ReplyAddress* inReply) {
    CallSequencedCommand(NotifyCmd, inWorld, inSize, inData, inReply);
    return kSCErr_None;
}

SCErr meth_error(World* inWorld, int inSize, char* inData, ReplyAddress* inReply);
SCErr meth_error(World* inWorld, int inSize, char* inData, ReplyAddress* /*inReply*/) {
    sc_msg_iter msg(inSize, inData);
    int mode = msg.geti();

    //	inWorld->mLocalErrorNotification = mode;
    //		// if non-zero, new state should be saved permanently
    //	if(mode) { inWorld->mErrorNotification = mode; };

    // -1 = bundle off, -2 = bundle on, 0 = permanent off, 1 = permanent on
    switch (mode) {
    case -1:
        inWorld->mLocalErrorNotification += 1;
        break;
    case -2:
        inWorld->mLocalErrorNotification -= 1;
        break;
    case 0:
        inWorld->mErrorNotification = 0;
        break;
    case 1:
        inWorld->mErrorNotification = 1;
    };
    return kSCErr_None;
}

#define NEW_COMMAND(name) NewCommand(#name, cmd_##name, meth_##name)

void initMiscCommands();
void initMiscCommands() {
    // nrt
    NEW_COMMAND(none);
    NEW_COMMAND(notify);
    NEW_COMMAND(status);
    NEW_COMMAND(quit);
    NEW_COMMAND(clearSched);
    NEW_COMMAND(version);
    NEW_COMMAND(rtMemoryStatus);

#ifdef CLOCKWORK_GUEST
#endif

    NEW_COMMAND(d_recv);
#ifndef SC_LEAN_TARGET
    NEW_COMMAND(d_load);
    NEW_COMMAND(d_loadDir);
#endif
    NEW_COMMAND(d_freeAll);
    NEW_COMMAND(d_free);

    NEW_COMMAND(s_new);
    NEW_COMMAND(s_newargs);

    NEW_COMMAND(n_trace);
    NEW_COMMAND(n_free);
    NEW_COMMAND(n_run);

    NEW_COMMAND(u_cmd);
    NEW_COMMAND(cmd);

    // NEW_COMMAND(n_cmd);
    NEW_COMMAND(n_map);
    NEW_COMMAND(n_mapn);
    NEW_COMMAND(n_mapa);
    NEW_COMMAND(n_mapan);
    NEW_COMMAND(n_set);
    NEW_COMMAND(n_setn);
    NEW_COMMAND(n_fill);

    NEW_COMMAND(n_before);
    NEW_COMMAND(n_after);
    NEW_COMMAND(n_order);

    NEW_COMMAND(g_new);
    NEW_COMMAND(g_head);
    NEW_COMMAND(g_tail);
    NEW_COMMAND(g_freeAll);
    NEW_COMMAND(g_deepFree);

    NEW_COMMAND(p_new);

    NEW_COMMAND(b_alloc);
    NEW_COMMAND(b_allocRead);
    NEW_COMMAND(b_allocPtr);
    NEW_COMMAND(b_allocReadChannel);

    NEW_COMMAND(b_read);
    NEW_COMMAND(b_readChannel);
    NEW_COMMAND(b_write);

    NEW_COMMAND(b_free);
    NEW_COMMAND(b_close);

    NEW_COMMAND(b_zero);
    NEW_COMMAND(b_set);
    NEW_COMMAND(b_setn);
    // =============================================================================
    // TAU: /b_setSampleRate intentionally not implemented
    // In WebAudio, the sample rate is fixed by the AudioContext, so all buffers
    // already have the correct sample rate. Setting buffer sample rate metadata
    // would have no useful effect in our context.
    // =============================================================================
    NEW_COMMAND(b_fill);
    NEW_COMMAND(b_gen);

    NEW_COMMAND(b_allocPtr);
    // The guest's own verbs, implemented beside the guest (piano_wavetable.cpp).
    NewCommand("supersonic/piano/wavetable", cmd_supersonic_piano_wavetable, supersonic_meth_piano_wavetable);
    NewCommand("supersonic/buffer/read", cmd_supersonic_buffer_read, meth_supersonic_buffer_read);
    NewCommand("supersonic/buffer/publish", cmd_supersonic_buffer_publish, meth_supersonic_buffer_publish);

    NEW_COMMAND(c_set);
    NEW_COMMAND(c_setn);
    NEW_COMMAND(c_fill);

    NEW_COMMAND(dumpOSC);

    NEW_COMMAND(c_get);
    NEW_COMMAND(c_getn);
    NEW_COMMAND(b_get);
    NEW_COMMAND(b_getn);
    NEW_COMMAND(s_get);
    NEW_COMMAND(s_getn);

    NEW_COMMAND(n_query);
    NEW_COMMAND(b_query);

    NEW_COMMAND(s_noid);
    NEW_COMMAND(sync);
    NEW_COMMAND(g_dumpTree);
    NEW_COMMAND(g_queryTree);

    NEW_COMMAND(error);
}
