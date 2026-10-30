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


#include "SC_SequencedCommand.h"
#include "SC_GraphDef.h"
#include "SC_BufGen.h"
#include "SC_CoreAudio.h"
#include "SC_Errors.h"
#include "scsynthsend.h"
#include "SC_Prototypes.h"
#include "SC_HiddenWorld.h"
#include "SC_Codecvt.hpp"
#include "SC_SndFileHelpers.hpp"
#include "sc_msg_iter.h"
#include "SC_WorldOptions.h"

// CLOCKWORK_GUEST: the engine has no files. /b_allocRead, /b_read,
// /b_readChannel, /b_allocReadChannel and /b_write are answered by the CLIENT
// at the gateway (SuperSonic's front natively, the client library on the
// web), which decodes or encodes on a thread of its own and moves frames
// through the lanes; what reaches the engine under those names is refused
// with a pointer to where the work is.

#include <cstring>
#include <cassert>
#include <filesystem>
#include <type_traits>

// Last, and deliberately: SC_Unit.h defines IN(index) and OUT(index), and a
// Windows header included after it fails to compile — winuser.h spells its
// parameters with the SAL macros IN and OUT, which by then mean something
// else. Nothing above needs SC_Unit.h (this file uses neither macro), and one
// of them reaches <windows.h> through the C++20 <chrono> that SC_Lock.h pulls
// in, which the C++17 build never did.
#include "SC_Unit.h"

// =============================================================================
// TAU MODIFICATIONS
// =============================================================================
// This file has the following changes from upstream SuperCollider:
//
// 1. scprintf: kept as upstream has it; it is the engine's one print, and on
//    every target it goes to the host through DspHost::log (SC_Stubs.cpp)
// 2. GET_COMPLETION_MSG: Removed optional integer skip (upstream commit b06dc8b4f)
//    - The upstream version skips an optional integer-tagged value after the completion message
//    - This affects all commands using GET_COMPLETION_MSG, not just /b_alloc
//    - Not needed in SuperSonic where these optional parameters are not used
// 3. BufAllocCmd::Init: Removed optional sampleRate parameter (upstream b06dc8b4f)
//    - In SuperSonic, the sample rate is fixed by the AudioContext
//    - We always use mWorld->mSampleRate for allocated buffers
// 4. BufFreeCmd::Stage4: Sends /supersonic/buffer/freed for JS buffer pool
// 5. AudioQuitCmd: Excludes mShmem and mQuitProgram (no SHM/semaphores in WASM)
// 6. RecvSynthDefCmd::Stage2: Null check for mDefs to prevent null pointer crash
// 7. RecvSynthDefCmd::Stage4: Sends /supersonic/synthdef/loaded messages
// 8. RecvSynthDefCmd::Init: Added scprintf for empty synthdef error
// 9. NotifyCmd::Stage2: /notify on an already-registered client replies /done
//    (idempotent) instead of upstream's /fail "already registered". Lets a
//    redundant re-registration succeed — needed because a device rebuild
//    (destroy_world/rebuild_world) preserves the notify clients, so a host that
//    also re-sends /notify afterwards must not get an error. See
//    capture_notify_clients/restore_notify_clients in audio_processor.cpp.
// =============================================================================

// From audio_processor.cpp
extern "C" int  supersonic_assets_contains(const void* ptr);
extern "C" void supersonic_guest_release_buffer(int bufnum);
extern "C" {
}

namespace fs = std::filesystem;

constexpr size_t ERR_BUF_SIZE = 512;

#define ReturnSCErrIfNil(ptr)                                                                                          \
    if (!ptr)                                                                                                          \
    return kSCErr_OutOfRealTimeMemory

#define ThrowIfNil(ptr)                                                                                                \
    if (!ptr)                                                                                                          \
        throw std::runtime_error(                                                                                      \
            std::string("alloc failed, increase server's memory allocation (e.g. via ServerOptions)"));

#define GET_COMPLETION_MSG(msg)                                                                                        \
    mMsgSize = msg.getbsize();                                                                                         \
    if (mMsgSize) {                                                                                                    \
        mMsgData = (char*)World_Alloc(mWorld, mMsgSize);                                                               \
        ReturnSCErrIfNil(mMsgData);                                                                                    \
        msg.getb(mMsgData, mMsgSize);                                                                                  \
    }

#define SEND_COMPLETION_MSG                                                                                            \
    if (mMsgSize) {                                                                                                    \
        OSC_Packet packet;                                                                                             \
        packet.mData = mMsgData;                                                                                       \
        packet.mSize = mMsgSize;                                                                                       \
        packet.mIsBundle = false;                                                                                      \
        packet.mReplyAddr = mReplyAddress;                                                                             \
        PacketStatus status = PerformCompletionMsg(mWorld, packet);                                                    \
        if (status == PacketScheduled) {                                                                               \
            mMsgSize = 0;                                                                                              \
            mMsgData = 0;                                                                                              \
        }                                                                                                              \
    }

// SuperSonic: the Buf*Cmd stages index mSndBufUpdates[mBufIndex] unchecked, and
// World_GetBuf / World_GetNRTBuf clamp an out-of-range index to 0, so Init
// rejects any mBufIndex outside [0, mNumSndBufs).
static bool BufIndexExists(World* inWorld, int inBufIndex) {
    return inBufIndex >= 0 && (uint32)inBufIndex < inWorld->mNumSndBufs;
}

void SndBuf_Init(SndBuf* buf) {
    buf->data = nullptr;
    buf->channels = 0;
    buf->samples = 0;
    buf->frames = 0;
    buf->mask = 0;
    buf->mask1 = 0;
    buf->coord = 0;
    buf->sndfile = nullptr;
}

/** @brief converts a path name to a std::filesystem::path,
 *  optionally restricting it to the given base directory.
 *  @param pathName The file path as a UTF-8 encoded C-string
 *  @param restrictBase (optional) base directory as UTF-8 encoded C-string
 *  @return a std::filesystem::path
 *  @throw std::runtime_error if the path is not inside the given base directory.
 */
fs::path createRestrictedPathFromCStr(const char* pathName, const char* restrictBase = nullptr) {
    assert(pathName != nullptr);
    auto path = SC_Codecvt::utf8_str_to_path(pathName);

    if (restrictBase != nullptr) {
        auto base = SC_Codecvt::utf8_str_to_path(restrictBase);
        auto relPath = fs::relative(path, base);
        if (!relPath.empty() && *relPath.begin() == "..")
            throw std::runtime_error(SC_Codecvt::path_to_utf8_str(path) + " is outside of "
                                     + SC_Codecvt::path_to_utf8_str(base) + "\n");
    }

    return path;
}

#ifndef NO_LIBSNDFILE

/** @brief try to open a sound file from an (optionally restricted) path.
 *  @throw std::runtime_error if the sound file could not be opened.
 */
SNDFILE* tryOpenSndfile(const char* pathName, int mode, SF_INFO* sfinfo, const char* restrictBase = nullptr) {
    auto path = createRestrictedPathFromCStr(pathName, restrictBase);
    SNDFILE* sf = sndfileOpenFromPath(path, mode, sfinfo);
    if (!sf) {
        char str[ERR_BUF_SIZE];
        snprintf(str, ERR_BUF_SIZE, "File '%s' could not be opend: %s\n", pathName, sf_strerror(nullptr));
        throw std::runtime_error(str);
    }
    return sf;
}

#endif // NO_LIBSNDFILE

///////////////////////////////////////////////////////////////////////////

SC_SequencedCommand::SC_SequencedCommand(World* inWorld, const ReplyAddress* inReplyAddress): mWorld(inWorld) {
    if (inReplyAddress)
        mReplyAddress = *inReplyAddress;
    else
        mReplyAddress.mReplyFunc = null_reply_func;
}

SC_SequencedCommand::~SC_SequencedCommand() {
    if (mMsgData)
        World_Free(mWorld, mMsgData);
}

int SC_SequencedCommand::Init(const char* /*inData*/, int /*inSize*/) { return kSCErr_None; }

void SC_SequencedCommand::SendDone(const char* inCommandName) { ::SendDone(&mReplyAddress, inCommandName); };

void SC_SequencedCommand::SendDoneWithIntValue(const char* inCommandName, int value) {
    ::SendDoneWithIntValue(&mReplyAddress, inCommandName, value);
}

void SC_SequencedCommand::SendFailure(const char* inCommandName, const char* errString) {
    ::SendFailure(&mReplyAddress, inCommandName, errString);
}

void SC_SequencedCommand::SendFailureWithIntValue(const char* inCommandName, const char* errString, int value) {
    ::SendFailureWithIntValue(&mReplyAddress, inCommandName, errString, value);
}

void SC_SequencedCommand::CallEveryStage() {
    switch (mNextStage) {
    case 1:
        if (!Stage1())
            break;
        mNextStage++;
    case 2:
        if (!Stage2())
            break;
        mNextStage++;
    case 3:
        if (!Stage3())
            break;
        mNextStage++;
    case 4:
        Stage4();
        break;
    }
    Delete();
}

void DoSequencedCommand(FifoMsg* inMsg) {
    SC_SequencedCommand* cmd = (SC_SequencedCommand*)inMsg->mData;
    cmd->CallNextStage();
}

void FreeSequencedCommand(FifoMsg* inMsg) {
    SC_SequencedCommand* cmd = (SC_SequencedCommand*)inMsg->mData;
    cmd->Delete();
}


void SC_SequencedCommand::CallNextStage() {
    bool sendAgain = false;
    FifoMsg msg;

    int isRealTime = mNextStage & 1;
    switch (mNextStage) {
    case 1:
        sendAgain = Stage1(); // RT
        break;
    case 2:
        sendAgain = Stage2(); // NRT
        break;
    case 3:
        sendAgain = Stage3(); // RT
        break;
    case 4:
        Stage4(); // NRT
        break;
    }
    mNextStage++;
    SC_AudioDriver* driver = AudioDriver(mWorld);
    if (sendAgain) {
        msg.Set(mWorld, DoSequencedCommand, nullptr, (void*)this);
        // send this to next time.
        if (isRealTime) {
            // send to NRT
            if (driver) driver->SendMsgFromEngine(msg);
        } else {
            // send to RT
            if (driver) driver->SendMsgToEngine(msg);
        }
    } else {
        if (isRealTime) {
            Delete();
        } else {
            // can only be freed from RT.
            if (driver) {
                msg.Set(mWorld, FreeSequencedCommand, nullptr, (void*)this);
                driver->SendMsgToEngine(msg);
            } else {
                Delete(); // In SAB/NRT mode without driver, free directly
            }
        }
    }
}

void SC_SequencedCommand::Delete() {
    std::destroy_at(this); // call (virtual) destructor
    World_Free(mWorld, this); // free memory
}

bool SC_SequencedCommand::Stage1() { return true; }

bool SC_SequencedCommand::Stage2() { return false; }

bool SC_SequencedCommand::Stage3() { return false; }

void SC_SequencedCommand::Stage4() {}


///////////////////////////////////////////////////////////////////////////

SyncCmd::SyncCmd(World* inWorld, const ReplyAddress* inReplyAddress): SC_SequencedCommand(inWorld, inReplyAddress) {}

int SyncCmd::Init(const char* inData, int inSize) {
    sc_msg_iter msg(inSize, inData);
    mID = msg.geti();
    return kSCErr_None;
}

bool SyncCmd::Stage2() { return true; }

bool SyncCmd::Stage3() { return true; }

void SyncCmd::Stage4() {
    small_scpacket packet;
    packet.adds("/synced");
    packet.maketags(2);
    packet.addtag(',');
    packet.addtag('i');

    packet.addi(mID);

    SendReply(&mReplyAddress, packet.data(), packet.size());
}

///////////////////////////////////////////////////////////////////////////

BufAllocCmd::BufAllocCmd(World* inWorld, const ReplyAddress* inReplyAddress):
    SC_SequencedCommand(inWorld, inReplyAddress) {}

int BufAllocCmd::Init(const char* inData, int inSize) {
    sc_msg_iter msg(inSize, inData);
    mBufIndex = msg.geti();
    if (!BufIndexExists(mWorld, mBufIndex))
        return kSCErr_IndexOutOfRange;
    mNumFrames = msg.geti();
    mNumChannels = msg.geti(1);

    GET_COMPLETION_MSG(msg);

    return kSCErr_None;
}

bool BufAllocCmd::Stage2() {
    SndBuf* buf = World_GetNRTBuf(mWorld, mBufIndex);
    mFreeData = buf->data;
    SCErr err = bufAlloc(buf, mNumChannels, mNumFrames, mWorld->mSampleRate);
    if (err) {
        scprintf("/b_alloc: memory allocation failed\n");
        return false;
    }
    mSndBuf = *buf;
    return true;
}

bool BufAllocCmd::Stage3() {
    SndBuf* buf = World_GetBuf(mWorld, mBufIndex);
    *buf = mSndBuf;
    mWorld->mSndBufUpdates[mBufIndex].writes++;
    SEND_COMPLETION_MSG;
    return true;
}

void BufAllocCmd::Stage4() {
    if (supersonic_assets_contains(mFreeData)) supersonic_guest_release_buffer(mBufIndex);
    else zfree(mFreeData);
    SendDoneWithIntValue("/b_alloc", mBufIndex);
}

///////////////////////////////////////////////////////////////////////////

#include "sc_msg_iter.h"
#include <string.h>

// Hand a finished block back to whoever allocated it (scsynth_dsp.cpp forwards
// to DspHost::free_bytes). A call, not a message — see BufFreeCmd::Stage4.
extern "C" void supersonic_guest_free_bytes(void* ptr);

BufGenCmd::BufGenCmd(World* inWorld, const ReplyAddress* inReplyAddress):
    SC_SequencedCommand(inWorld, inReplyAddress) {}

BufGenCmd::~BufGenCmd() { World_Free(mWorld, mData); }

int BufGenCmd::Init(const char* inData, int inSize) {
    mSize = inSize;
    mData = (char*)World_Alloc(mWorld, mSize);
    ReturnSCErrIfNil(mData);
    memcpy(mData, inData, mSize);

    sc_msg_iter msg(mSize, mData);
    mBufIndex = msg.geti();
    if (!BufIndexExists(mWorld, mBufIndex))
        return kSCErr_IndexOutOfRange;

    int32* genName = msg.gets4();
    if (!genName)
        return kSCErr_WrongArgType;

    mBufGen = GetBufGen(genName);
    if (!mBufGen)
        return kSCErr_BufGenNotFound;

    mMsg = msg;

    return kSCErr_None;
}

bool BufGenCmd::Stage2() {
    SndBuf* buf = World_GetNRTBuf(mWorld, mBufIndex);

    mFreeData = buf->data;
    (*mBufGen->mBufGenFunc)(mWorld, buf, &mMsg);
    if (buf->data == mFreeData)
        mFreeData = nullptr;
    mSndBuf = *buf;
    return true;
}

bool BufGenCmd::Stage3() {
    SndBuf* buf = World_GetBuf(mWorld, mBufIndex);
    *buf = mSndBuf;
    mWorld->mSndBufUpdates[mBufIndex].writes++;
    return true;
}

void BufGenCmd::Stage4() {
    if (supersonic_assets_contains(mFreeData)) supersonic_guest_release_buffer(mBufIndex);
    else zfree(mFreeData);
    SendDoneWithIntValue("/b_gen", mBufIndex);
}

///////////////////////////////////////////////////////////////////////////


BufFreeCmd::BufFreeCmd(World* inWorld, const ReplyAddress* inReplyAddress):
    SC_SequencedCommand(inWorld, inReplyAddress) {}

int BufFreeCmd::Init(const char* inData, int inSize) {
    sc_msg_iter msg(inSize, inData);
    mBufIndex = msg.geti();
    if (!BufIndexExists(mWorld, mBufIndex))
        return kSCErr_IndexOutOfRange;

    GET_COMPLETION_MSG(msg);

    return kSCErr_None;
}

bool BufFreeCmd::Stage2() {
    SndBuf* buf = World_GetNRTBuf(mWorld, mBufIndex);
    mFreeData = buf->data;
#ifndef NO_LIBSNDFILE
    if (buf->sndfile)
        sf_close(GETSNDFILE(buf));
#endif
    SndBuf_Init(buf);
    return true;
}

bool BufFreeCmd::Stage3() {
    SndBuf* buf = World_GetBuf(mWorld, mBufIndex);

    SndBuf_Init(buf);
    mWorld->mSndBufUpdates[mBufIndex].writes++;
    SEND_COMPLETION_MSG;

    return true;
}

void BufFreeCmd::Stage4() {
    // Hand the memory back: a heap block to the host's allocator, an asset
    // to the client that committed it, which hears /clockwork/asset/released
    // and reclaims its slot. The same on every platform.
    if (mFreeData) {
        /*
         * Hand the block back to whoever allocated it, by CALL not by message.
         *
         * The comment above said "Native: SupersonicEngine intercepts and calls
         * zfree()" — an OSC verb the host had to string-match. The two sides
         * stopped agreeing (the host came to match "/clockwork/buffer/freed"
         * while this still sent "/supersonic/buffer/freed") and every freed
         * block leaked on native, silently, because a match that fails looks
         * exactly like a message that was not for you.
         */
        supersonic_guest_free_bytes(mFreeData);
        // …and if the bytes were an asset in the assets area, that is what lets go
        // of it: the client hears /clockwork/asset/released and reclaims.
        if (supersonic_assets_contains(mFreeData)) supersonic_guest_release_buffer(mBufIndex);

    }
    SendDoneWithIntValue("/b_free", mBufIndex);
}

///////////////////////////////////////////////////////////////////////////

BufZeroCmd::BufZeroCmd(World* inWorld, const ReplyAddress* inReplyAddress):
    SC_SequencedCommand(inWorld, inReplyAddress) {}

int BufZeroCmd::Init(const char* inData, int inSize) {
    sc_msg_iter msg(inSize, inData);
    mBufIndex = msg.geti();
    if (!BufIndexExists(mWorld, mBufIndex))
        return kSCErr_IndexOutOfRange;

    GET_COMPLETION_MSG(msg);

    return kSCErr_None;
}

bool BufZeroCmd::Stage2() {
    SndBuf* buf = World_GetNRTBuf(mWorld, mBufIndex);
    std::fill_n(buf->data, buf->samples, 0);
    return true;
}

bool BufZeroCmd::Stage3() {
    mWorld->mSndBufUpdates[mBufIndex].writes++;
    SEND_COMPLETION_MSG;
    return true;
}

void BufZeroCmd::Stage4() { SendDoneWithIntValue("/b_zero", mBufIndex); }

///////////////////////////////////////////////////////////////////////////

BufAllocReadCmd::BufAllocReadCmd(World* inWorld, const ReplyAddress* inReplyAddress):
    SC_SequencedCommand(inWorld, inReplyAddress) {}

int BufAllocReadCmd::Init(const char* inData, int inSize) {
    sc_msg_iter msg(inSize, inData);
    mBufIndex = msg.geti();
    if (!BufIndexExists(mWorld, mBufIndex))
        return kSCErr_IndexOutOfRange;

    const char* filename = msg.gets();
    if (!filename)
        return kSCErr_WrongArgType;

    auto size = strlen(filename) + 1; // include null-terminator
    mFilename = (char*)World_Alloc(mWorld, size);
    ReturnSCErrIfNil(mFilename);
    memcpy(mFilename, filename, size);

    mFileOffset = msg.geti();
    mNumFrames = msg.geti();

    GET_COMPLETION_MSG(msg);

    return kSCErr_None;
}

BufAllocReadCmd::~BufAllocReadCmd() { World_Free(mWorld, mFilename); }

bool BufAllocReadCmd::Stage2() {
    // NOT ANSWERED HERE — see the note at the top of this file.
    SendFailureWithIntValue("/b_allocRead", "the engine does not read files: decode into the assets area and commit it as an asset (/clockwork/asset/commit), or send /b_allocPtr\n", mBufIndex);
    return false;
}

bool BufAllocReadCmd::Stage3() {
    SndBuf* buf = World_GetBuf(mWorld, mBufIndex);
    *buf = mSndBuf;
    mWorld->mSndBufUpdates[mBufIndex].writes++;
    SEND_COMPLETION_MSG;

    return true;
}

void BufAllocReadCmd::Stage4() {
    if (supersonic_assets_contains(mFreeData)) supersonic_guest_release_buffer(mBufIndex);
    else zfree(mFreeData);
    SendDoneWithIntValue("/b_allocRead", mBufIndex);
}

///////////////////////////////////////////////////////////////////////////

BufReadCmd::BufReadCmd(World* inWorld, const ReplyAddress* inReplyAddress):
    SC_SequencedCommand(inWorld, inReplyAddress) {}

int BufReadCmd::Init(const char* inData, int inSize) {
    sc_msg_iter msg(inSize, inData);
    mBufIndex = msg.geti();
    if (!BufIndexExists(mWorld, mBufIndex))
        return kSCErr_IndexOutOfRange;

    const char* filename = msg.gets();
    if (!filename)
        return kSCErr_WrongArgType;

    auto size = strlen(filename) + 1; // include null-terminator
    mFilename = (char*)World_Alloc(mWorld, size);
    ReturnSCErrIfNil(mFilename);
    memcpy(mFilename, filename, size);

    mFileOffset = msg.geti();
    mNumFrames = msg.geti(-1);
    mBufOffset = msg.geti();
    mLeaveFileOpen = msg.geti();

    GET_COMPLETION_MSG(msg);

    return kSCErr_None;
}

BufReadCmd::~BufReadCmd() { World_Free(mWorld, mFilename); }

bool BufReadCmd::Stage2() {
    // NOT ANSWERED HERE — see the note at the top of this file.
    SendFailureWithIntValue("/b_read", "the engine does not read files: a client decodes into the assets area and sends /supersonic/buffer/read\n", mBufIndex);
    return false;
}

bool BufReadCmd::Stage3() {
    SndBuf* buf = World_GetBuf(mWorld, mBufIndex);
    buf->samplerate = mSampleRate;
    if (mLeaveFileOpen)
        buf->mask = buf->mask1 = -1;

    mWorld->mSndBufUpdates[mBufIndex].writes++;
    SEND_COMPLETION_MSG;
    return true;
}

void BufReadCmd::Stage4() { SendDoneWithIntValue("/b_read", mBufIndex); }

///////////////////////////////////////////////////////////////////////////

BufChannelCommand::BufChannelCommand(World* inWorld, const ReplyAddress* inReplyAddress):
    SC_SequencedCommand(inWorld, inReplyAddress) {}

BufChannelCommand::~BufChannelCommand() { World_Free(mWorld, mChannels); }

void BufChannelCommand::InitChannels(sc_msg_iter& msg) {
    assert(mNumChannels == 0 && mChannelCapacity == 0);

    while (msg.nextTag(0) == 'i') {
        int c = msg.geti();
        if (mNumChannels >= mChannelCapacity) {
            if (mChannelCapacity == 0) {
                // first allocation
                mChannelCapacity = 8; // initial capacity
                mChannels = (int*)World_Alloc(mWorld, mChannelCapacity * sizeof(int));
            } else {
                // grow capacity and reallocate
                mChannelCapacity *= 2;
                mChannels = (int*)World_Realloc(mWorld, mChannels, mChannelCapacity * sizeof(int));
            }
            ThrowIfNil(mChannels);
        }
        mChannels[mNumChannels++] = c;
    }
}

bool BufChannelCommand::CheckChannels(int inNumChannels) const {
    for (int i = 0; i < mNumChannels; ++i) {
        if (mChannels[i] < 0 || mChannels[i] >= inNumChannels) {
            return false;
        }
    }
    return true;
}

void BufChannelCommand::CopyChannels(const float* src, size_t srcChannels, float* dst, size_t numFrames) const {
    for (int ci = 0; ci < mNumChannels; ++ci) {
        int c = mChannels[ci];
        if (c >= 0 && c < srcChannels) {
            for (size_t fi = 0; fi < numFrames; ++fi) {
                dst[fi * mNumChannels + ci] = src[fi * srcChannels + c];
            }
        } else {
            for (size_t fi = 0; fi < numFrames; ++fi) {
                dst[fi * mNumChannels + ci] = 0.f;
            }
        }
    }
}

///////////////////////////////////////////////////////////////////////////

BufAllocReadChannelCmd::BufAllocReadChannelCmd(World* inWorld, const ReplyAddress* inReplyAddress):
    BufChannelCommand(inWorld, inReplyAddress) {}

int BufAllocReadChannelCmd::Init(const char* inData, int inSize) {
    sc_msg_iter msg(inSize, inData);
    mBufIndex = msg.geti();
    if (!BufIndexExists(mWorld, mBufIndex))
        return kSCErr_IndexOutOfRange;

    const char* filename = msg.gets();
    if (!filename)
        return kSCErr_WrongArgType;

    auto size = strlen(filename) + 1; // include null-terminator
    mFilename = (char*)World_Alloc(mWorld, size);
    ReturnSCErrIfNil(mFilename);
    memcpy(mFilename, filename, size);

    mFileOffset = msg.geti();
    mNumFrames = msg.geti();

    InitChannels(msg);

    GET_COMPLETION_MSG(msg);

    return kSCErr_None;
}

BufAllocReadChannelCmd::~BufAllocReadChannelCmd() { World_Free(mWorld, mFilename); }

bool BufAllocReadChannelCmd::Stage2() {
    // NOT ANSWERED HERE — see the note at the top of this file.
    SendFailureWithIntValue("/b_allocReadChannel", "the engine does not read files: decode into the assets area and commit it as an asset (/clockwork/asset/commit), or send /b_allocPtr\n", mBufIndex);
    return false;
}

bool BufAllocReadChannelCmd::Stage3() {
    SndBuf* buf = World_GetBuf(mWorld, mBufIndex);
    *buf = mSndBuf;
    mWorld->mSndBufUpdates[mBufIndex].writes++;
    SEND_COMPLETION_MSG;

    return true;
}

void BufAllocReadChannelCmd::Stage4() {
    if (supersonic_assets_contains(mFreeData)) supersonic_guest_release_buffer(mBufIndex);
    else zfree(mFreeData);
    SendDoneWithIntValue("/b_allocReadChannel", mBufIndex);
}

///////////////////////////////////////////////////////////////////////////

BufReadChannelCmd::BufReadChannelCmd(World* inWorld, const ReplyAddress* inReplyAddress):
    BufChannelCommand(inWorld, inReplyAddress) {}

int BufReadChannelCmd::Init(const char* inData, int inSize) {
    sc_msg_iter msg(inSize, inData);
    mBufIndex = msg.geti();
    if (!BufIndexExists(mWorld, mBufIndex))
        return kSCErr_IndexOutOfRange;

    const char* filename = msg.gets();
    if (!filename)
        return kSCErr_WrongArgType;

    auto size = strlen(filename) + 1; // include null-terminator
    mFilename = (char*)World_Alloc(mWorld, size);
    ReturnSCErrIfNil(mFilename);
    memcpy(mFilename, filename, size);

    mFileOffset = msg.geti();
    mNumFrames = msg.geti(-1);
    mBufOffset = msg.geti();
    mLeaveFileOpen = msg.geti();

    InitChannels(msg);

    GET_COMPLETION_MSG(msg);

    return kSCErr_None;
}

BufReadChannelCmd::~BufReadChannelCmd() { World_Free(mWorld, mFilename); }

bool BufReadChannelCmd::Stage2() {
    // NOT ANSWERED HERE — see the note at the top of this file.
    SendFailureWithIntValue("/b_readChannel", "the engine does not read files: a client decodes into the assets area and sends /supersonic/buffer/read\n", mBufIndex);
    return false;
}

bool BufReadChannelCmd::Stage3() {
    SndBuf* buf = World_GetBuf(mWorld, mBufIndex);
    buf->samplerate = mSampleRate;
    if (mLeaveFileOpen)
        buf->mask = buf->mask1 = -1;

    mWorld->mSndBufUpdates[mBufIndex].writes++;
    SEND_COMPLETION_MSG;
    return true;
}

void BufReadChannelCmd::Stage4() { SendDoneWithIntValue("/b_readChannel", mBufIndex); }

///////////////////////////////////////////////////////////////////////////

BufWriteCmd::BufWriteCmd(World* inWorld, const ReplyAddress* inReplyAddress):
    SC_SequencedCommand(inWorld, inReplyAddress) {}


int BufWriteCmd::Init(const char* inData, int inSize) {
    // Parsed for its shape only; Stage2 refuses. The header and sample
    // format names are the client's concern now, at the gateway.
    sc_msg_iter msg(inSize, inData);
    mBufIndex = msg.geti();
    if (!BufIndexExists(mWorld, mBufIndex))
        return kSCErr_IndexOutOfRange;
    const char* filename = msg.gets();
    if (!filename)
        return kSCErr_WrongArgType;

    auto size = strlen(filename) + 1; // include null-terminator
    mFilename = (char*)World_Alloc(mWorld, size);
    ReturnSCErrIfNil(mFilename);
    memcpy(mFilename, filename, size);

    (void)msg.gets("aiff");
    (void)msg.gets("int16");

    mNumFrames = msg.geti(-1);
    mBufOffset = msg.geti();
    mLeaveFileOpen = msg.geti();
    GET_COMPLETION_MSG(msg);
    return kSCErr_None;
}

BufWriteCmd::~BufWriteCmd() { World_Free(mWorld, mFilename); }

bool BufWriteCmd::Stage2() {
    // NOT ANSWERED HERE — see the note at the top of this file.
    SendFailureWithIntValue("/b_write", "the engine does not write files: /supersonic/buffer/publish puts the frames in the returns area for a client to encode\n", mBufIndex);
    return false;
}

bool BufWriteCmd::Stage3() {
    SEND_COMPLETION_MSG;
    return true;
}

void BufWriteCmd::Stage4() { SendDoneWithIntValue("/b_write", mBufIndex); }

///////////////////////////////////////////////////////////////////////////

BufCloseCmd::BufCloseCmd(World* inWorld, const ReplyAddress* inReplyAddress):
    SC_SequencedCommand(inWorld, inReplyAddress) {}

int BufCloseCmd::Init(const char* inData, int inSize) {
    sc_msg_iter msg(inSize, inData);
    mBufIndex = msg.geti();
    if (!BufIndexExists(mWorld, mBufIndex))
        return kSCErr_IndexOutOfRange;

    GET_COMPLETION_MSG(msg);

    return kSCErr_None;
}

bool BufCloseCmd::Stage2() {
#ifdef NO_LIBSNDFILE
    SendFailure("/b_close", "scsynth compiled without libsndfile\n");
    scprintf("scsynth compiled without libsndfile\n");
    return false;
#else
    SndBuf* buf = World_GetNRTBuf(mWorld, mBufIndex);
    if (buf->sndfile) {
        sf_close(GETSNDFILE(buf));
        buf->sndfile = nullptr;
    }
    return true;
#endif
}

bool BufCloseCmd::Stage3() {
    SEND_COMPLETION_MSG;
    return true;
}

void BufCloseCmd::Stage4() { SendDoneWithIntValue("/b_close", mBufIndex); }

///////////////////////////////////////////////////////////////////////////

AudioQuitCmd::AudioQuitCmd(World* inWorld, const ReplyAddress* inReplyAddress):
    SC_SequencedCommand(inWorld, inReplyAddress) {}

#if SC_AUDIO_API == SC_AUDIO_API_AUDIOUNITS

bool AudioQuitCmd::Stage1() { return true; }

bool AudioQuitCmd::Stage2() {
    SendFailure("/quit", "not allowed in AU host\n");
    scprintf("/quit : quit not allowed in AU host\n");
    return false;
}

#else

bool AudioQuitCmd::Stage1() {
    if (!mWorld->hw->mTerminating) {
        mWorld->hw->mTerminating = true;
        mDidQuit = true;
    }
    return true;
}

bool AudioQuitCmd::Stage2() {
    if (mDidQuit) {
        SendDone("/quit");
#ifndef SC_LEAN_TARGET
        mWorld->hw->mQuitProgram->post();
#endif
    } else {
        scprintf("/quit: already quitting\n");
        SendFailure("/quit", "already quitting\n");
    }
    return false;
}

#endif

///////////////////////////////////////////////////////////////////////////

NotifyCmd::NotifyCmd(World* inWorld, const ReplyAddress* inReplyAddress):
    SC_SequencedCommand(inWorld, inReplyAddress) {}

int NotifyCmd::Init(const char* inData, int inSize) {
    sc_msg_iter msg(inSize, inData);
    mOnOff = msg.geti();
    mID = msg.geti(-1);

    return kSCErr_None;
}

/** \brief Attempts to find and remove the requested \c id from \c availableIDs.
 *
 * If \c id is -1 or not in \c availableIDs, returns the first element in
 * \c availableIDs. Otherwise, returns \c id.
 */
int popAvailableClientID(int const id, ClientIDs& availableIDs) {
    int clientID = -1;
    if (id == -1) {
        // no requested clientID
        clientID = availableIDs.front(); // pop an ID
        availableIDs.pop_front();
    } else {
        // user ID requested
        auto it = std::find(availableIDs.begin(), availableIDs.end(), id);
        if (it != availableIDs.end()) { // return the requested ID if available
            clientID = id;
            availableIDs.erase(it);
        } else {
            // otherwise return the first free one
            clientID = availableIDs.front();
            availableIDs.pop_front();
        }
    }

    return clientID;
}

bool NotifyCmd::Stage2() {
    HiddenWorld* hw = mWorld->hw;

    if (mOnOff) {
        for (auto addr : *hw->mUsers) {
            if (mReplyAddress == addr) {
                // Already registered — idempotent: reply /done with the existing
                // clientID rather than failing. DIVERGES FROM UPSTREAM scsynth,
                // which replies /fail "already registered" here. A redundant
                // /notify (e.g. a host re-registering after a device rebuild that
                // already preserved the registration) must succeed, not error.
                SendDoneWithVarArgs(&mReplyAddress, "/notify", "ii",
                                    hw->mClientIDdict->at(mReplyAddress), (int)hw->mMaxUsers);
                return false;
            }
        }

        if (hw->mUsers->size() >= hw->mMaxUsers) {
            SendFailure("/notify", "too many users\n");
            scprintf("/notify: too many users\n");
            return false;
        }

        int const clientID = popAvailableClientID(mID, *hw->mAvailableClientIDs);

        hw->mClientIDdict->emplace(mReplyAddress, clientID);
        hw->mUsers->insert(mReplyAddress);
        SendDoneWithVarArgs(&mReplyAddress, "/notify", "ii", clientID, (int)hw->mMaxUsers);

    } else {
        auto const it = std::find(hw->mUsers->begin(), hw->mUsers->end(), mReplyAddress);
        if (it != hw->mUsers->end()) {
            // remove from list
            hw->mAvailableClientIDs->push_back(hw->mClientIDdict->at(mReplyAddress)); // push the freed ID
            hw->mClientIDdict->erase(mReplyAddress);
            hw->mUsers->erase(it);
            SendDone("/notify");
            return false;
        }

        SendFailure("/notify", "not registered\n");
        scprintf("/notify: not registered\n");
    }
    return false;
}

///////////////////////////////////////////////////////////////////////////

SendFailureCmd::SendFailureCmd(World* inWorld, const ReplyAddress* inReplyAddress, const char* inCmdName,
                               const char* inErrString):
    SC_SequencedCommand(inWorld, inReplyAddress) {
    mCmdName = (char*)World_Alloc(mWorld, strlen(inCmdName) + 1);
    ThrowIfNil(mCmdName);
    strcpy(mCmdName, inCmdName);

    mErrString = (char*)World_Alloc(mWorld, strlen(inErrString) + 1);
    ThrowIfNil(mErrString);
    strcpy(mErrString, inErrString);
}

SendFailureCmd::~SendFailureCmd() {
    World_Free(mWorld, mCmdName);
    World_Free(mWorld, mErrString);
}

bool SendFailureCmd::Stage2() {
    SendFailure(mCmdName, mErrString);
    return false;
}

///////////////////////////////////////////////////////////////////////////

RecvSynthDefCmd::RecvSynthDefCmd(World* inWorld, const ReplyAddress* inReplyAddress):
    SC_SequencedCommand(inWorld, inReplyAddress) {}

int RecvSynthDefCmd::Init(const char* inData, int inSize) {
    sc_msg_iter msg(inSize, inData);

    mSize = msg.getbsize();
    if (!mSize) {
        scprintf("ERROR /d_recv: synthdef data is empty");
        throw kSCErr_WrongArgType;
    }

    mBuffer = (char*)World_Alloc(mWorld, mSize);
    ReturnSCErrIfNil(mBuffer);
    msg.getb(mBuffer, mSize);

    GET_COMPLETION_MSG(msg);

    mDefs = nullptr;
    return kSCErr_None;
}

RecvSynthDefCmd::~RecvSynthDefCmd() { World_Free(mWorld, mBuffer); }

bool RecvSynthDefCmd::Stage2() {
    mDefs = GraphDef_Recv(mWorld, mBuffer, mSize, mDefs, &mErrorMsg);

    if (mDefs) {
        return true;
    } else {
        // Send /fail with the error message so clients know what went wrong
        if (!mErrorMsg.empty()) {
            SendFailure("/d_recv", mErrorMsg.c_str());
        } else {
            SendFailure("/d_recv", "failed to load synthdef");
        }
        return false;  // Abort command - don't proceed to Stage3 with null pointer
    }
}

bool RecvSynthDefCmd::Stage3() {
    GraphDef_Define(mWorld, mDefs);
    SEND_COMPLETION_MSG;
    return true;
}

void RecvSynthDefCmd::Stage4() {
    SendDone("/d_recv");

#ifdef SC_LEAN_TARGET
    // TAU: Send /supersonic/synthdef/loaded for each synthdef that was loaded
    // This allows JavaScript to track which synthdefs are available
    // A synthdef file can contain multiple definitions (linked list via mNext)
    GraphDef* def = mDefs;
    while (def) {
        small_scpacket packet;
        packet.adds("/supersonic/synthdef/loaded");
        packet.maketags(2);
        packet.addtag(',');
        packet.addtag('s');  // synthdef name
        packet.adds((char*)def->mNodeDef.mName);
        SendReply(&mReplyAddress, packet.data(), packet.size());
        def = def->mNext;
    }
#endif
}

///////////////////////////////////////////////////////////////////////////

#ifndef SC_LEAN_TARGET
LoadSynthDefCmd::LoadSynthDefCmd(World* inWorld, const ReplyAddress* inReplyAddress):
    SC_SequencedCommand(inWorld, inReplyAddress) {}

int LoadSynthDefCmd::Init(const char* inData, int inSize) {
    sc_msg_iter msg(inSize, inData);

    const char* filename = msg.gets();
    if (!filename)
        return kSCErr_WrongArgType;

    auto size = strlen(filename) + 1; // include null-terminator
    mFilename = (char*)World_Alloc(mWorld, size);
    ReturnSCErrIfNil(mFilename);
    memcpy(mFilename, filename, size);

    GET_COMPLETION_MSG(msg);

    mDefs = nullptr;
    return kSCErr_None;
}

LoadSynthDefCmd::~LoadSynthDefCmd() { World_Free(mWorld, mFilename); }

bool LoadSynthDefCmd::Stage2() {
    try {
        auto path = createRestrictedPathFromCStr(mFilename, mWorld->mRestrictedPath);
        mDefs = GraphDef_LoadGlob(mWorld, SC_Codecvt::path_to_utf8_str(path).c_str(), mDefs);
        if (!mDefs) {
            char str[ERR_BUF_SIZE];
            snprintf(str, ERR_BUF_SIZE, "File '%s' could not be opened\n", mFilename);
            throw std::runtime_error(str);
        }
    } catch (const std::exception& e) {
        SendFailure("/d_load", e.what());
        scprintf(e.what());
        return false;
    }

    return true;
}

bool LoadSynthDefCmd::Stage3() {
    GraphDef_Define(mWorld, mDefs);
    SEND_COMPLETION_MSG;
    return true;
}

void LoadSynthDefCmd::Stage4() { SendDone("/d_load"); }

///////////////////////////////////////////////////////////////////////////

LoadSynthDefDirCmd::LoadSynthDefDirCmd(World* inWorld, const ReplyAddress* inReplyAddress):
    SC_SequencedCommand(inWorld, inReplyAddress) {}

int LoadSynthDefDirCmd::Init(const char* inData, int inSize) {
    sc_msg_iter msg(inSize, inData);

    const char* filename = msg.gets();
    if (!filename)
        return kSCErr_WrongArgType;

    auto size = strlen(filename) + 1; // include null-terminator
    mFilename = (char*)World_Alloc(mWorld, size);
    ReturnSCErrIfNil(mFilename);
    memcpy(mFilename, filename, size);

    GET_COMPLETION_MSG(msg);

    mDefs = nullptr;
    return kSCErr_None;
}

LoadSynthDefDirCmd::~LoadSynthDefDirCmd() { World_Free(mWorld, mFilename); }

bool LoadSynthDefDirCmd::Stage2() {
    try {
        if (!std::filesystem::exists(mFilename)) {
            char str[ERR_BUF_SIZE];
            snprintf(str, ERR_BUF_SIZE, "Could not load synthdefs. Directory '%s' does not exist\n", mFilename);
            throw std::runtime_error(str);
        }

        auto path = createRestrictedPathFromCStr(mFilename, mWorld->mRestrictedPath);
        mDefs = GraphDef_LoadDir(mWorld, path, mDefs);
        if (!mDefs) {
            // QUESTION: should this really be an error or could this be a warning?
            char str[ERR_BUF_SIZE];
            snprintf(str, ERR_BUF_SIZE, "No synthdefs found in directory: '%s'\n", mFilename);
            throw std::runtime_error(str);
        }
    } catch (const std::exception& e) {
        SendFailure("/d_loadDir", e.what());
        scprintf(e.what());
        return false;
    }

    return true;
}

bool LoadSynthDefDirCmd::Stage3() {
    GraphDef_Define(mWorld, mDefs);
    SEND_COMPLETION_MSG;
    return true;
}

void LoadSynthDefDirCmd::Stage4() { SendDone("/d_loadDir"); }
#endif // !SC_LEAN_TARGET

///////////////////////////////////////////////////////////////////////////

SendReplyCmd::SendReplyCmd(World* inWorld, const ReplyAddress* inReplyAddress):
    SC_SequencedCommand(inWorld, inReplyAddress) {}

int SendReplyCmd::Init(const char* inData, int inSize) {
    // the message data will be deallocated in ~SC_SequencedCommand.
    mMsgSize = inSize;
    mMsgData = (char*)World_Alloc(mWorld, mMsgSize);
    ReturnSCErrIfNil(mMsgData);
    memcpy(mMsgData, inData, inSize);
    return kSCErr_None;
}

bool SendReplyCmd::Stage2() {
    SendReply(&mReplyAddress, mMsgData, mMsgSize);
    return false;
}

///////////////////////////////////////////////////////////////////////////

SCErr PerformAsynchronousCommand(
    World* inWorld, void* replyAddr, const char* cmdName, void* cmdData,
    AsyncStageFn stage2, // stage2 is non real time
    AsyncStageFn stage3, // stage3 is real time - completion msg performed if stage3 returns true
    AsyncStageFn stage4, // stage4 is non real time - sends done if stage4 returns true
    AsyncFreeFn cleanup, int32 completionMsgSize, const void* completionMsgData) {
    void* space = World_Alloc(inWorld, sizeof(AsyncPlugInCmd));
    ReturnSCErrIfNil(space);
    AsyncPlugInCmd* cmd = new (space) AsyncPlugInCmd(inWorld, (ReplyAddress*)replyAddr, cmdName, cmdData, stage2,
                                                     stage3, stage4, cleanup, completionMsgSize, completionMsgData);
    if (!cmd)
        return kSCErr_Failed;
    if (inWorld->mRealTime)
        cmd->CallNextStage();
    else
        cmd->CallEveryStage();
    return kSCErr_None;
}

///////////////////////////////////////////////////////////////////////////

SCErr PerformAsynchronousCommandEx(
    World* inWorld, void* replyAddr, const char* cmdName, void* cmdData,
    AsyncStageFnEx stage2, // stage2 is non real time
    AsyncStageFnEx stage3, // stage3 is real time - completion msg performed if stage3 returns true
    AsyncStageFnEx stage4, // stage4 is non real time - sends done if stage4 returns true
    AsyncFreeFn cleanup, int completionMsgSize, const void* completionMsgData) {
    void* space = World_Alloc(inWorld, sizeof(AsyncPlugInCmdEx));
    ReturnSCErrIfNil(space);
    AsyncPlugInCmdEx* cmd = new (space) AsyncPlugInCmdEx(inWorld, (ReplyAddress*)replyAddr, cmdName, cmdData, stage2,
                                                         stage3, stage4, cleanup, completionMsgSize, completionMsgData);
    if (!cmd)
        return kSCErr_Failed;
    if (inWorld->mRealTime)
        cmd->CallNextStage();
    else
        cmd->CallEveryStage();
    return kSCErr_None;
}

///////////////////////////////////////////////////////////////////////////

template <typename StageFn>
AsyncPlugInCmd_<StageFn>::AsyncPlugInCmd_(
    World* inWorld, const ReplyAddress* inReplyAddress, const char* cmdName, void* cmdData,
    StageFn stage2, // stage2 is non real time
    StageFn stage3, // stage3 is real time - completion msg performed if stage3 returns true
    StageFn stage4, // stage4 is non real time - sends done if stage4 returns true
    AsyncFreeFn cleanup, // cleanup is called in real time
    int completionMsgSize, const void* completionMsgData):
    SC_SequencedCommand(inWorld, inReplyAddress),
    mCmdName(cmdName),
    mCmdData(cmdData),
    mStage2(stage2),
    mStage3(stage3),
    mStage4(stage4),
    mCleanup(cleanup) {
    if (completionMsgSize > 0 && completionMsgData) {
        mMsgSize = completionMsgSize;
        mMsgData = (char*)World_Alloc(mWorld, mMsgSize);
        ThrowIfNil(mMsgData);
        memcpy(mMsgData, completionMsgData, mMsgSize);
    }
}

template <typename StageFn> AsyncPlugInCmd_<StageFn>::~AsyncPlugInCmd_() {
    // NOTE: before SC 3.15 the clean up function could not be NULL.
    if (mCleanup)
        mCleanup(mWorld, mCmdData);
}

template <typename StageFn> bool AsyncPlugInCmd_<StageFn>::Stage2() {
    bool result;
    if constexpr (std::is_same_v<StageFn, AsyncStageFnEx>)
        result = !mStage2 || (mStage2)(mWorld, mCmdData, &mReplyAddress);
    else
        result = !mStage2 || (mStage2)(mWorld, mCmdData);
    return result;
}

template <typename StageFn> bool AsyncPlugInCmd_<StageFn>::Stage3() {
    bool result;
    if constexpr (std::is_same_v<StageFn, AsyncStageFnEx>)
        result = !mStage3 || (mStage3)(mWorld, mCmdData, &mReplyAddress);
    else
        result = !mStage3 || (mStage3)(mWorld, mCmdData);
    if (result)
        SEND_COMPLETION_MSG;
    return result;
}

template <typename StageFn> void AsyncPlugInCmd_<StageFn>::Stage4() {
    bool result;
    if constexpr (std::is_same_v<StageFn, AsyncStageFnEx>)
        result = !mStage4 || (mStage4)(mWorld, mCmdData, &mReplyAddress);
    else
        result = !mStage4 || (mStage4)(mWorld, mCmdData);
    if (result && mCmdName && mReplyAddress.mReplyFunc != null_reply_func)
        SendDone(mCmdName);
}

///////////////////////////////////////////////////////////////////////////

SCErr PerformAsyncUnitCommand(
    Unit* inUnit, void* replyAddr, const char* cmdName, void* cmdData,
    AsyncUnitStageFn stage2, // stage2 is non real time
    AsyncUnitStageFn stage3, // stage3 is real time - completion msg performed if stage3 returns true
    AsyncUnitStageFn stage4, // stage4 is non real time - sends done if stage4 returns true
    AsyncFreeFn cleanup, int completionMsgSize, const void* completionMsgData) {
    if (!Graph_HasParent(inUnit->mParent)) {
        // If Graph_HasParent() returns false, it means that the graph has been removed. This can only
        // happen if DoAsyncUnitCommand() is called in a Unit destructor (which is not allowed).
        // This check is important because it makes sure that we don't increment a reference count
        // that has already gone to zero!
        scprintf("ERROR: cannot call DoAsyncUnitCommand() in a Unit destructor!\n");
        return kSCErr_Failed;
    }

    void* space = World_Alloc(inUnit->mWorld, sizeof(AsyncUnitCmd));
    ReturnSCErrIfNil(space);
    AsyncUnitCmd* cmd = new (space) AsyncUnitCmd(inUnit, (ReplyAddress*)replyAddr, cmdName, cmdData, stage2, stage3,
                                                 stage4, cleanup, completionMsgSize, completionMsgData);
    if (!cmd)
        return kSCErr_Failed;
    if (inUnit->mWorld->mRealTime)
        cmd->CallNextStage();
    else
        cmd->CallEveryStage();
    return kSCErr_None;
}

///////////////////////////////////////////////////////////////////////////

AsyncUnitCmd::AsyncUnitCmd(
    Unit* inUnit, const ReplyAddress* inReplyAddress, const char* cmdName, void* cmdData,
    AsyncUnitStageFn stage2, // stage2 is non real time
    AsyncUnitStageFn stage3, // stage3 is real time - completion msg performed if stage3 returns true
    AsyncUnitStageFn stage4, // stage4 is non real time - sends done if stage4 returns true
    AsyncFreeFn cleanup, // cleanup is called in real time
    int completionMsgSize, const void* completionMsgData):
    SC_SequencedCommand(inUnit->mWorld, inReplyAddress),
    mUnit(inUnit),
    mCmdName(cmdName),
    mCmdData(cmdData),
    mStage2(stage2),
    mStage3(stage3),
    mStage4(stage4),
    mCleanup(cleanup) {
    // make sure that the owning Graph is kept alive for the whole duration of the command!
    Graph_AddRef(inUnit->mParent);

    if (completionMsgSize > 0 && completionMsgData) {
        mMsgSize = completionMsgSize;
        mMsgData = (char*)World_Alloc(mWorld, mMsgSize);
        ThrowIfNil(mMsgData);
        memcpy(mMsgData, completionMsgData, mMsgSize);
    }
}

AsyncUnitCmd::~AsyncUnitCmd() {
    if (mCleanup)
        mCleanup(mWorld, mCmdData);
    // finally release the owning Graph
    Graph_Release(mUnit->mParent);
}

bool AsyncUnitCmd::Stage2() {
    bool result = !mStage2 || (mStage2)(mUnit, mCmdData, &mReplyAddress);
    return result;
}

bool AsyncUnitCmd::Stage3() {
    // Check whether the owning Graph has been removed in the meantime. If yes, we pass
    // nullptr as the Unit pointer so that the stage function can detect and properly
    // handle the situation. For example, it may still have to release resources on stage4.
    mAlive = Graph_HasParent(mUnit->mParent);
    bool result = !mStage3 || (mStage3)(mAlive ? mUnit : nullptr, mCmdData, &mReplyAddress);
    // Only send completition message if the Graph is still alive!
    if (result && mAlive)
        SEND_COMPLETION_MSG;
    return result;
}

void AsyncUnitCmd::Stage4() {
    bool result = !mStage4 || (mStage4)(mUnit, mCmdData, &mReplyAddress);
    // Only send /done message if the Graph has been alive in stage3.
    if (result && mAlive && mCmdName && mReplyAddress.mReplyFunc != null_reply_func)
        SendDone(mCmdName);
}
