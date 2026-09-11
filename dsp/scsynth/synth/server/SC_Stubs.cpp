/*
 * SC_Stubs.cpp - Supersonic implementations of scsynth functions
 *
 * Contains both active runtime code (OSC processing, debug output, timing)
 * and minimal linker stubs for unused subsystems (audio driver, TCP/IP).
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

// Deterministic-seed hook for offline renders.
//
// server_timeseed() is the only clock-dependent input to the DSP graph: it
// seeds World::mRGen at boot and backs InterfaceTable::fRanSeed, so two
// otherwise identical renders of a graph containing any RNG ugen diverge. A
// host that needs reproducible renders (nrt/nrt_render.cpp — the golden-render
// harness) sets this to a non-zero base before clockwork_init; seeds are then handed
// out as base, base+1, base+2, … in call order, which is fixed for a fixed
// boot + score. Zero (the default, and what every shipping host leaves it at)
// keeps the historic clock seeding, so real-time behaviour is unchanged.
extern "C" {
int32_t g_ss_deterministic_seed = 0;
}

int32 server_timeseed() {
    if (g_ss_deterministic_seed != 0) {
        static int32 counter = 0;
        return g_ss_deterministic_seed + counter++;
    }
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
// OSC processing - based on SC_CoreAudio.cpp reference implementation
// ============================================================================

// From audio_processor.cpp (WASM) or native layer
extern "C" {
    int clockwork_log(const char* fmt, ...);
    int clockwork_log_va(const char* fmt, va_list args);
    int clockwork_log_raw(const char* msg, uint32_t len);
}

// ============================================================================
// OSC dump for /dumpOSC command - uses clockwork_log instead of scprintf
// ============================================================================

static void dumpOSCtoDebug(int mode, int inSize, char* inData, const char* prefix = "dumpOSC: ") {
    if (mode & 1) {
        // Mode 1: Parsed OSC message
        char buf[1024];
        int pos = 0;
        const char* data;
        int size;

        pos += snprintf(buf + pos, sizeof(buf) - pos, "%s[ ", prefix);
        if (inData[0]) {
            // String command
            pos += snprintf(buf + pos, sizeof(buf) - pos, "\"%s\"", inData);
            data = OSCstrskip(inData);
            size = inSize - (data - inData);
        } else {
            // Integer command
            pos += snprintf(buf + pos, sizeof(buf) - pos, "%d", OSCint(inData));
            data = inData + 4;
            size = inSize - 4;
        }

        sc_msg_iter msg(size, data);
        while (msg.remain() && pos < (int)sizeof(buf) - 64) {
            char c = msg.nextTag('i');
            switch (c) {
            case 'i': pos += snprintf(buf + pos, sizeof(buf) - pos, ", %d", msg.geti()); break;
            case 'f': pos += snprintf(buf + pos, sizeof(buf) - pos, ", %g", msg.getf()); break;
            case 'd': pos += snprintf(buf + pos, sizeof(buf) - pos, ", %g", msg.getd()); break;
            case 's': pos += snprintf(buf + pos, sizeof(buf) - pos, ", \"%s\"", msg.gets()); break;
            case 'b': pos += snprintf(buf + pos, sizeof(buf) - pos, ", DATA[%zu]", msg.getbsize()); msg.skipb(); break;
            case 'T': pos += snprintf(buf + pos, sizeof(buf) - pos, ", true"); msg.count++; break;
            case 'F': pos += snprintf(buf + pos, sizeof(buf) - pos, ", false"); msg.count++; break;
            case 'N': pos += snprintf(buf + pos, sizeof(buf) - pos, ", nil"); msg.count++; break;
            case '[': pos += snprintf(buf + pos, sizeof(buf) - pos, ", ["); msg.count++; break;
            case ']': pos += snprintf(buf + pos, sizeof(buf) - pos, " ]"); msg.count++; break;
            default: pos += snprintf(buf + pos, sizeof(buf) - pos, ", ?"); break;
            }
        }
        pos += snprintf(buf + pos, sizeof(buf) - pos, " ]\n");
        clockwork_log_raw(buf, pos);
    }

    if (mode & 2) {
        // Mode 2: Hex dump (first 64 bytes)
        char buf[512];
        int pos = 0;
        int dumpSize = inSize > 64 ? 64 : inSize;
        pos += snprintf(buf + pos, sizeof(buf) - pos, "OSC HEX (%d bytes): ", inSize);
        for (int i = 0; i < dumpSize && pos < (int)sizeof(buf) - 4; i++) {
            pos += snprintf(buf + pos, sizeof(buf) - pos, "%02x ", (unsigned char)inData[i]);
        }
        if (inSize > 64) {
            pos += snprintf(buf + pos, sizeof(buf) - pos, "...");
        }
        pos += snprintf(buf + pos, sizeof(buf) - pos, "\n");
        clockwork_log_raw(buf, pos);
    }
}

// PerformOSCMessage - dispatch OSC commands to their handlers
// Based on SC_CoreAudio.cpp:200-224
int PerformOSCMessage(World* inWorld, int inSize, char* inData, ReplyAddress* inReply) {
    // Validate inputs
    if (!inWorld) {
        clockwork_log("ERROR: PerformOSCMessage called with null World");
        return kSCErr_Failed;
    }
    if (!inData) {
        clockwork_log("ERROR: PerformOSCMessage called with null data");
        return kSCErr_Failed;
    }
    if (inSize <= 0 || inSize > 65536) {
        clockwork_log("ERROR: PerformOSCMessage invalid size: %d", inSize);
        return kSCErr_Failed;
    }

    // Safety check: ensure command library is initialized
    if (!gCmdLib) {
        clockwork_log("ERROR: gCmdLib not initialized");
        return kSCErr_Failed;
    }

    // Dump OSC message if enabled via /dumpOSC command
    if (inWorld->mDumpOSC) {
        dumpOSCtoDebug(inWorld->mDumpOSC, inSize, inData);
    }

    SC_LibCmd* cmdObj;
    int cmdNameLen;

    if (inData[0] == 0) {
        // Integer command (first byte is 0)
        cmdNameLen = 4;
        if (inSize < 4) {
            clockwork_log("ERROR: Integer command too short: %d bytes", inSize);
            return kSCErr_Failed;
        }
        uint32 index = inData[3];
        if (index >= NUMBER_OF_COMMANDS)
            cmdObj = nullptr;
        else
            cmdObj = gCmdArray[index];
    } else {
        // String command (like "/status")
        cmdNameLen = OSCstrlen(inData);
        if (cmdNameLen <= 0 || cmdNameLen > inSize) {
            clockwork_log("ERROR: Invalid command name length: %d (data size: %d)", cmdNameLen, inSize);
            return kSCErr_Failed;
        }
        cmdObj = gCmdLib->Get((int32*)inData);
    }

    if (!cmdObj) {
        dumpOSCtoDebug(1, inSize, inData, "Command not found: ");
        return kSCErr_NoSuchCommand;
    }

    // Validate arguments size
    int argSize = inSize - cmdNameLen;
    if (argSize < 0) {
        clockwork_log("ERROR: Negative argument size: %d (cmd=%d, total=%d)", argSize, cmdNameLen, inSize);
        return kSCErr_Failed;
    }

    // Execute command via exception-safe Perform() wrapper
    // Pass only the arguments (skip the command name)
    int err = cmdObj->Perform(inWorld, argSize, inData + cmdNameLen, inReply);

    return err;
}

// Maximum bundle nesting depth - prevents stack overflow from malicious packets
static constexpr int MAX_BUNDLE_DEPTH = 8;

// Internal helper with depth tracking
static void PerformOSCBundleWithDepth(World* inWorld, OSC_Packet* inPacket, int depth) {
    // Depth limit check - prevents stack overflow from deeply nested bundles
    if (depth > MAX_BUNDLE_DEPTH) {
        clockwork_log("ERROR: Bundle nesting too deep (%d > %d), skipping",
                     depth, MAX_BUNDLE_DEPTH);
        return;
    }

    // Validate inputs
    if (!inWorld) {
        clockwork_log("ERROR: PerformOSCBundle called with null World");
        return;
    }
    if (!inPacket || !inPacket->mData) {
        clockwork_log("ERROR: PerformOSCBundle called with null packet/data");
        return;
    }
    if (inPacket->mSize < 16) {
        clockwork_log("ERROR: Bundle too small: %d bytes (min 16)", inPacket->mSize);
        return;
    }
    if (inPacket->mSize > 65536) {
        clockwork_log("ERROR: Bundle too large: %d bytes", inPacket->mSize);
        return;
    }

    char* data = inPacket->mData + 16;  // Skip "#bundle" (8 bytes) + timetag (8 bytes)
    char* dataEnd = inPacket->mData + inPacket->mSize;
    int msgCount = 0;
    const int maxMessages = 256;  // Sanity limit

    while (data < dataEnd && msgCount < maxMessages) {
        // Check we have at least 4 bytes for size
        if (data + 4 > dataEnd) {
            clockwork_log("ERROR: Bundle truncated at message %d (need 4 bytes, have %ld)",
                         msgCount, (long)(dataEnd - data));
            break;
        }

        // Read big-endian int32 message size
        int32_t msgSize = ((uint8_t)data[0] << 24) |
                          ((uint8_t)data[1] << 16) |
                          ((uint8_t)data[2] << 8) |
                          ((uint8_t)data[3]);
        data += sizeof(int32_t);

        // Validate message size
        if (msgSize <= 0) {
            clockwork_log("ERROR: Invalid message size %d at message %d", msgSize, msgCount);
            break;
        }
        if (msgSize > 65536) {
            clockwork_log("ERROR: Message %d too large: %d bytes", msgCount, msgSize);
            break;
        }
        if (data + msgSize > dataEnd) {
            clockwork_log("ERROR: Message %d overflows bundle (size=%d, avail=%ld)",
                         msgCount, msgSize, (long)(dataEnd - data));
            break;
        }

        // Check if this is a nested bundle (starts with "#bundle")
        if (msgSize >= 8 && strncmp(data, "#bundle", 7) == 0) {
            // Recursively handle nested bundle with incremented depth
            OSC_Packet nestedPacket;
            nestedPacket.mData = data;
            nestedPacket.mSize = msgSize;
            nestedPacket.mIsBundle = true;
            nestedPacket.mReplyAddr = inPacket->mReplyAddr;
            PerformOSCBundleWithDepth(inWorld, &nestedPacket, depth + 1);
        } else {
            // Perform the OSC message
            PerformOSCMessage(inWorld, msgSize, data, &inPacket->mReplyAddr);
        }
        data += msgSize;
        msgCount++;
    }

    if (msgCount >= maxMessages) {
        clockwork_log("WARNING: Bundle hit message limit (%d)", maxMessages);
    }

    // Reset error notification state for next command
    inWorld->mLocalErrorNotification = 0;
}

// PerformOSCBundle - execute all messages in an OSC bundle
// Based on SC_CoreAudio.cpp:226-243
void PerformOSCBundle(World* inWorld, OSC_Packet* inPacket) {
    PerformOSCBundleWithDepth(inWorld, inPacket, 0);
}

// ProcessOSCPacket - handle OSC commands in NRT mode
// Based on SC_CoreAudio.cpp:179-198, but simplified for NRT (no FIFO/threading)
bool ProcessOSCPacket(World* inWorld, OSC_Packet* inPacket) {
    if (!inWorld || !inPacket || !inPacket->mData) {
        clockwork_log("ERROR: ProcessOSCPacket called with null pointers");
        return false;
    }

    // Validate World structure
    if (!inWorld->hw) {
        clockwork_log("ERROR: World->hw is null");
        return false;
    }
    if (!inWorld->hw->mAllocPool) {
        clockwork_log("ERROR: World->hw->mAllocPool is null");
        return false;
    }

    // Debug: Log the OSC command
    // const char* cmdName = inPacket->mData;
    // if (cmdName[0] == '/') {
    //     char msg[128];
    //     snprintf(msg, sizeof(msg), "ProcessOSCPacket: %s (size=%d)", cmdName, inPacket->mSize);
    //     clockwork_log(msg);
    // }

    // In NRT mode, directly call PerformOSCMessage (no FIFO/threading needed)
    int err = PerformOSCMessage(inWorld, inPacket->mSize, inPacket->mData, &inPacket->mReplyAddr);

    // NOTE: Do NOT free inPacket->mData here - the caller (SC_OscUnroll.cpp) handles that

    if (err != kSCErr_None) {
        char msg[128];
        snprintf(msg, sizeof(msg), "ProcessOSCPacket: Command returned error %d, continuing", err);
        clockwork_log(msg);
    }

    // clockwork_log("ProcessOSCPacket: Returning true (continue processing)");

    // IMPORTANT: Return true even on error - we've reported the error, now continue processing
    // Returning false would stop the audio processing loop
    return true;
}

// PerformCompletionMsg - completion messages in NRT mode
// This is called by sequenced commands after Stage3 to execute completion messages
// In NRT mode we execute synchronously and return PacketPerformed
PacketStatus PerformCompletionMsg(World* inWorld, const OSC_Packet& inPacket) {
    if (!inPacket.mData || inPacket.mSize <= 0) {
        clockwork_log("PerformCompletionMsg: empty completion message");
        return PacketPerformed;
    }

    // Check if it's a bundle: first 8 bytes are "#bundle\0"
    bool isBundle = (inPacket.mSize >= 16
        && inPacket.mData[0] == '#'
        && inPacket.mData[1] == 'b'
        && inPacket.mData[2] == 'u'
        && inPacket.mData[3] == 'n');

    if (isBundle) {
        OSC_Packet packet = inPacket;
        packet.mIsBundle = true;
        PerformOSCBundle(inWorld, &packet);
    } else {
        PerformOSCMessage(inWorld, inPacket.mSize, inPacket.mData,
                          const_cast<ReplyAddress*>(&inPacket.mReplyAddr));
    }

    return PacketPerformed;
}

// ASIO thread stubs — SuperSonic uses ring buffer (WASM) or UdpOscTransport
// (native) instead of boost::asio for OSC transport. Always no-ops.
namespace scsynth {
void startAsioThread() {}
void stopAsioThread() {}
bool asioThreadStarted() { return false; }
}

// scprintf — used by various upstream SC code paths (GraphDef loading,
// SequencedCommand, the RT pool's placement report). Defined for every target:
// it is a wrapper over clockwork_log_va, which every target has, and it was inside
// the native-only block below only because nothing shared used to reach it.
int scprintf(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    int ret = clockwork_log_va(fmt, args);
    va_end(args);
    return ret;
}

// ============================================================================
// Native-only stubs
// ============================================================================
#ifndef __EMSCRIPTEN__

// DiskIO + UIUGens stubs — not built for SuperSonic native backend.
#include "SC_InterfaceTable.h"
extern "C" void DiskIO_Load(InterfaceTable*) {}
void DiskIO_Unload() {}
void UIUGens_Unload() {}

#endif // !__EMSCRIPTEN__

// ============================================================================
// Audio driver stubs — dead code, and deliberately so.
// ============================================================================
// The World runs mRealTime = false on every host: scsynth-nrt is driven by
// clockwork's audio ticks and owns no thread, so every sequenced command runs
// its stages inline. The driver is never created; SC_World.cpp references it
// inside `if (world->mRealTime)` blocks the linker still has to resolve.
//
// NOT the place to reintroduce scsynth's real-time mode. Its off-thread stages
// are what a second thread would buy, and that is the guest's execution model
// changing under a year of design that assumes it does not. Work that must
// leave the audio thread — decoding a sample file — belongs in front of the
// engine, the way the web client answers /b_allocRead with /b_allocPtr.

SC_AudioDriver* SC_NewAudioDriver(World* world) {
    (void)world;
    return nullptr;  // Never called when mRealTime = false
}

bool SC_AudioDriver::Setup() { return false; }
bool SC_AudioDriver::Start() { return false; }
bool SC_AudioDriver::Stop()  { return false; }
bool SC_AudioDriver::SendMsgToEngine(FifoMsg& inMsg)   { (void)inMsg; return false; }
bool SC_AudioDriver::SendMsgFromEngine(FifoMsg& inMsg) { (void)inMsg; return false; }
