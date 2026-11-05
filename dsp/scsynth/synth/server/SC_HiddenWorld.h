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


#pragma once

#include "SC_Types.h"
#include "scsynth_config.h"
#include "SC_Rate.h"
#include "SC_SndBuf.h"
#include "SC_RGen.h"
#include "HashTable.h"
#include "SC_World.h"
#include "SC_Wire.h"
#include "SC_Reply.h"
#include "MsgFifo.h"
#include "guest_heap.h"
#include "SC_InterfaceTable.h"   // the World's copy of the table
#include "SC_Lib_Cintf.h"        // SC_LibCmd, NUMBER_OF_COMMANDS
#include "SC_SynthDef.h"         // NodeDef
#include <map>
#include <deque>
#include <set>

#ifndef SC_LEAN_TARGET
#include "SC_QuitSemaphore.hpp"
#endif

#ifndef NO_LIBSNDFILE
#    include <SC_SndFileHelpers.hpp> // includes sndfile.h with appropriate configuration
#endif


struct TriggerMsg {
    World* mWorld;
    int32 mNodeID;
    int32 mTriggerID;
    float mValue;

    void Perform();
};


struct NodeReplyMsg {
    World* mWorld;
    int32 mNodeID;
    int32 mID;
    int32 mNumArgs;
    float* mValues;
    int32 mCmdNameSize;
    char* mCmdName;
    void* mRTMemory;
    void Perform();
};


struct NodeEndMsg {
    World* mWorld;
    int32 mNodeID;
    int32 mGroupID;
    int32 mPrevNodeID;
    int32 mNextNodeID;
    int32 mIsGroup;
    int32 mHeadID;
    int32 mTailID;
    int32 mState;

    void Perform();
};

// Frees a list of GraphDefs (linked by mNext) and sends /d_removed for each
// to every registered client. Upstream sends it to the NRT thread through a
// FIFO in real-time mode; here every stage runs on one thread, so the release
// performs it directly and there is no FIFO.
struct DeleteGraphDefMsg {
    World* mWorld;
    struct GraphDef* mDefList;

    void Perform();
};

// Fixed-capacity notification FIFOs, allocated inline in HiddenWorld. At the
// default depths they dominate sizeof(HiddenWorld) (~120 KiB on 64-bit); the
// depths come from memory_profile.h and default to upstream scsynth's fixed
// sizes. MsgFifo enforces the power-of-two / minimum-depth invariant itself.
typedef MsgFifoNoFree<TriggerMsg, SC_TRIGGERS_FIFO_SIZE> TriggersFifo;
typedef MsgFifoNoFree<NodeReplyMsg, SC_NODE_REPLY_FIFO_SIZE> NodeReplyFifo;
typedef MsgFifoNoFree<NodeEndMsg, SC_NODE_ENDS_FIFO_SIZE> NodeEndsFifo;
// The instance's tables and client lists, in the guest's own memory
// (guest_heap.h): upstream's are on the system allocator.
typedef HashTable<struct GraphDef, GuestMalloc> GrafDefTable;

typedef std::map<struct ReplyAddress, uint32, std::less<struct ReplyAddress>,
                 GuestAllocator<std::pair<const struct ReplyAddress, uint32>>> ClientIDDict;
typedef std::deque<int, GuestAllocator<int>> ClientIDs;
typedef std::set<ReplyAddress, std::less<ReplyAddress>, GuestAllocator<ReplyAddress>> Clients;

struct HiddenWorld {
    // SuperSonic (plural guest): the instance of the guest this World belongs to (scsynth_dsp.cpp):
    // every door to the host (guest_doors.h) finds it through the World.
    struct SupersonicGuest* mGuest;
    // SuperSonic (plural guest): the instance's heap, for the tables (guest_heap.h).
    GuestMalloc mGuestMalloc;
    // SuperSonic (plural guest): the real-time pool's first area, pre-sized at World_New and handed over
    // when the pool asks (rt_pool_area_alloc); and that area when it lies in
    // the host's working memory, which is the host's and is never freed.
    void* mRtPoolFirstArea;
    void* mRtPoolArenaArea;
    // SuperSonic (plural guest): the seeds of this World's random generators (server_timeseed): from
    // mSeedBase upwards when a host pinned one (WorldOptions::mDeterministicSeed),
    // else from the clock, mixed with the count.
    int32 mSeedBase;
    int32 mSeedCounter;
    // SuperSonic (plural guest): the node id the last lookup failed on, for the error's text (SC_ErrorString).
    int32 mMissingNodeID;
    // SuperSonic (plural guest): Node_SendReply's dropped replies, each kind logged five times.
    uint32 mSendReplyAllocFails;
    uint32 mSendReplyFifoFulls;
    // SuperSonic (plural guest): Group_DumpNodeTree's indentation while it recurses through the calc funcs.
    int32 mDumpTabCount;
    // SuperSonic (plural guest): the definition parser's reason for refusing, for a catch that cannot ask
    // the exception (GraphDef_Recv).
    char mLastGraphDefError[256];
    // SuperSonic (plural guest): the registries upstream keeps one set of per process, one set per
    // World: built by initialize_library when the World is made, from the
    // plugins' loads, and freed by deinitialize_library. The World's copy of
    // the interface table names it (mWorld), so a load registers here.
    InterfaceTable mInterfaceTable;
    HashTable<class SC_LibCmd, GuestMalloc>* mCmdLib;
    class SC_LibCmd* mCmdArray[NUMBER_OF_COMMANDS];
    HashTable<struct UnitDef, GuestMalloc>* mUnitDefLib;
    HashTable<struct BufGen, GuestMalloc>* mBufGenLib;
    HashTable<struct PlugInCmd, GuestMalloc>* mPlugInCmds;
    NodeDef mGroupNodeDef;

    class AllocPool* mAllocPool;
    IntHashTable<struct Node, AllocPool>* mNodeLib;
    GrafDefTable* mGraphDefLib;
    uint32 mMaxUsers;
    Clients* mUsers;
    ClientIDs* mAvailableClientIDs;
    ClientIDDict* mClientIDdict;

    class SC_AudioDriver* mAudioDriver;
    char mPassword[32];

    uint32 mMaxWireBufs;
    float* mWireBufSpace;
    Wire mMissingInputWire;

    TriggersFifo mTriggers;
    NodeReplyFifo mNodeMsgs;
    NodeEndsFifo mNodeEnds;

#ifndef SC_LEAN_TARGET
    sc::sync::semaphore* mQuitProgram;
#endif
    bool mTerminating;

#ifndef NO_LIBSNDFILE
    SNDFILE* mNRTInputFile;
    SNDFILE* mNRTOutputFile;
    FILE* mNRTCmdFile;
#endif

    int32 mHiddenID;
    int32 mRecentID;

#ifdef __APPLE__
    const char* mInputStreamsEnabled;
    const char* mOutputStreamsEnabled;
#endif

#ifdef SC_BELA
    uint32 mBelaAnalogInputChannels;
    uint32 mBelaAnalogOutputChannels;
    uint32 mBelaDigitalChannels;
    float mBelaHeadphoneLevel;
    float mBelaPgaGainLeft;
    float mBelaPgaGainRight;
    bool mBelaSpeakerMuted;
    float mBelaDacLevel;
    float mBelaAdcLevel;
    uint32 mBelaNumMuxChannels;
    uint32 mBelaPru;
#endif

    const char* mInDeviceName;
    const char* mOutDeviceName;
};

typedef struct HiddenWorld HiddenWorld;

inline SC_AudioDriver* AudioDriver(World* inWorld) { return inWorld->hw->mAudioDriver; }
