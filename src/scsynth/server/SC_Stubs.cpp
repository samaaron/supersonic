/*
 * SC_Stubs.cpp - Minimal stub implementations for scsynth NRT mode
 *
 * These functions are referenced by scsynth code but never executed
 * when running in NRT (non-realtime) mode with mRealTime = false.
 * They exist purely to satisfy the linker.
 */

#include "SC_CoreAudio.h"
#include "SC_WorldOptions.h"
#include "SC_Time.hpp"
#include "OSC_Packet.h"
#include "SC_Errors.h"
#include "SC_World.h"
#include "SC_HiddenWorld.h"
#include "SC_Lib_Cintf.h"
#include "SC_OSC_Commands.h"
#include "sc_msg_iter.h"
#include <stdint.h>
#include <cstddef>
#include <cstring>
#include <cstdio>
#include <cstdarg>

// ============================================================================
// Runtime stubs ACTUALLY USED in NRT mode
// ============================================================================

int32 server_timeseed() {
    // This IS called - needs real implementation
    return timeSeed();
}

void initializeScheduler() {
    // Empty - scheduler not needed in NRT mode (externally driven)
}

int64 oscTimeNow() {
    // Used for OSC timestamps
    return OSCTime(getTime());
}

// ============================================================================
// Memory allocator stubs (DEAD CODE in NRT mode)
// ============================================================================
// These are referenced in realtime code paths that never execute in NRT
// Leaving as extern "C" declarations so other code can reference them

extern "C" {
    void* malloc_ex(size_t size, void* mem_pool) {
        return nullptr;  // Never called in NRT
    }

    void free_ex(void* ptr, void* mem_pool) {
        // Never called in NRT
    }

    size_t init_memory_pool(size_t mem_pool_size, void* mem_pool) {
        return 0;  // Never called in NRT
    }
}

// ============================================================================
// OSC processing - based on SC_CoreAudio.cpp reference implementation
// ============================================================================

// From audio_processor.cpp
extern "C" {
    int worklet_debug(const char* fmt, ...);
}

// PerformOSCMessage - dispatch OSC commands to their handlers
// Based on SC_CoreAudio.cpp:200-224
int PerformOSCMessage(World* inWorld, int inSize, char* inData, ReplyAddress* inReply) {
    // Safety check: ensure command library is initialized
    if (!gCmdLib) {
        worklet_debug("ERROR: gCmdLib not initialized");
        return kSCErr_Failed;
    }

    SC_LibCmd* cmdObj;
    int cmdNameLen;

    if (inData[0] == 0) {
        // Integer command (first byte is 0)
        cmdNameLen = 4;
        uint32 index = inData[3];
        if (index >= NUMBER_OF_COMMANDS)
            cmdObj = nullptr;
        else
            cmdObj = gCmdArray[index];
    } else {
        // String command (like "/status")
        cmdNameLen = OSCstrlen(inData);
        cmdObj = gCmdLib->Get((int32*)inData);
    }

    if (!cmdObj) {
        char msg[128];
        snprintf(msg, sizeof(msg), "Command not found: %s", inData);
        worklet_debug(msg);
        return kSCErr_NoSuchCommand;
    }

    // Execute command via exception-safe Perform() wrapper
    // Pass only the arguments (skip the command name)
    int err = cmdObj->Perform(inWorld, inSize - cmdNameLen, inData + cmdNameLen, inReply);

    return err;
}

// PerformOSCBundle - execute all messages in an OSC bundle
// Based on SC_CoreAudio.cpp:226-243
void PerformOSCBundle(World* inWorld, OSC_Packet* inPacket) {
    char* data = inPacket->mData + 16;  // Skip "#bundle" (8 bytes) + timetag (8 bytes)
    char* dataEnd = inPacket->mData + inPacket->mSize;

    while (data < dataEnd) {
        // Read big-endian int32 message size
        int32_t msgSize = ((uint8_t)data[0] << 24) |
                          ((uint8_t)data[1] << 16) |
                          ((uint8_t)data[2] << 8) |
                          ((uint8_t)data[3]);
        data += sizeof(int32_t);

        // Perform the OSC message
        PerformOSCMessage(inWorld, msgSize, data, &inPacket->mReplyAddr);
        data += msgSize;
    }

    // Reset error notification state for next command
    inWorld->mLocalErrorNotification = 0;
}

// ProcessOSCPacket - handle OSC commands in NRT mode
// Based on SC_CoreAudio.cpp:179-198, but simplified for NRT (no FIFO/threading)
bool ProcessOSCPacket(World* inWorld, OSC_Packet* inPacket) {
    if (!inWorld || !inPacket || !inPacket->mData) {
        worklet_debug("ERROR: ProcessOSCPacket called with null pointers");
        return false;
    }

    // Validate World structure
    if (!inWorld->hw) {
        worklet_debug("ERROR: World->hw is null");
        return false;
    }
    if (!inWorld->hw->mAllocPool) {
        worklet_debug("ERROR: World->hw->mAllocPool is null");
        return false;
    }

    // Debug: Log the OSC command
    // const char* cmdName = inPacket->mData;
    // if (cmdName[0] == '/') {
    //     char msg[128];
    //     snprintf(msg, sizeof(msg), "ProcessOSCPacket: %s (size=%d)", cmdName, inPacket->mSize);
    //     worklet_debug(msg);
    // }

    // In NRT mode, directly call PerformOSCMessage (no FIFO/threading needed)
    int err = PerformOSCMessage(inWorld, inPacket->mSize, inPacket->mData, &inPacket->mReplyAddr);

    // NOTE: Do NOT free inPacket->mData here - the caller (audio_processor.cpp) handles that

    if (err != kSCErr_None) {
        char msg[128];
        snprintf(msg, sizeof(msg), "ProcessOSCPacket: Command returned error %d, continuing", err);
        worklet_debug(msg);
    }

    // worklet_debug("ProcessOSCPacket: Returning true (continue processing)");

    // IMPORTANT: Return true even on error - we've reported the error, now continue processing
    // Returning false would stop the audio processing loop
    return true;
}

// PerformCompletionMsg - completion messages in NRT mode
// This is called by sequenced commands after Stage3 to execute completion messages
// In RT mode this would schedule the message for later execution
// In NRT mode we execute synchronously, so just return PacketPerformed
PacketStatus PerformCompletionMsg(World* inWorld, const OSC_Packet& inPacket) {
    // In NRT mode, completion messages are processed synchronously via UnrollOSCPacket
    // We don't need to schedule them - just acknowledge they were handled
    worklet_debug("PerformCompletionMsg: completion message processed");
    return PacketPerformed;
}

// ============================================================================
// Audio driver stubs (DEAD CODE in NRT mode)
// ============================================================================
// The audio driver is only created when mRealTime = true
// All these are in dead code paths guarded by if (world->mRealTime)

SC_AudioDriver* SC_NewAudioDriver(World* world) {
    return nullptr;  // Never called when mRealTime = false
}

// SC_AudioDriver method stubs
// These are referenced in SC_World.cpp inside if (world->mRealTime) blocks
// They will never be called, but the linker needs them

bool SC_AudioDriver::Setup() {
    return false;  // Never called in NRT
}

bool SC_AudioDriver::Start() {
    return false;  // Never called in NRT
}

bool SC_AudioDriver::SendMsgToEngine(FifoMsg& inMsg) {
    return false;  // Never called in NRT
}

bool SC_AudioDriver::SendMsgFromEngine(FifoMsg& inMsg) {
    return false;  // Never called in NRT
}
