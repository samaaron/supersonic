/*
 * SC_OscUnroll.cpp - OSC bundle unpacking for WASM
 *
 * Extracted from SC_ComPort.cpp to avoid nova-tt threading dependencies
 * This file contains only UnrollOSCPacket which recursively unpacks OSC bundles
 */

#include "SC_Endian.h"
#include "SC_HiddenWorld.h"
#include "OSC_Packet.h"
#include "sc_msg_iter.h"
#include <cstring>
#include <cstdlib>

// Forward declaration - implemented in SC_Stubs.cpp
bool ProcessOSCPacket(World* inWorld, OSC_Packet* inPacket);

namespace scsynth {

// UnrollOSCPacket - recursively unpack OSC bundles into individual messages
// From SC_ComPort.cpp:58-143
bool UnrollOSCPacket(World* inWorld, int inSize, char* inData, OSC_Packet* inPacket) {
    if (!strcmp(inData, "#bundle")) { // is a bundle
        char* data;
        char* dataEnd = inData + inSize;
        int len = 16;
        bool hasNestedBundle = false;

        // get len of nested messages only, without len of nested bundle(s)
        data = inData + 16; // skip bundle header
        while (data < dataEnd) {
            int32 msgSize = OSCint(data);
            data += sizeof(int32);
            if (strcmp(data, "#bundle")) // is a message
                len += sizeof(int32) + msgSize;
            else
                hasNestedBundle = true;
            data += msgSize;
        }

        if (hasNestedBundle) {
            if (len > 16) { // not an empty bundle
                // add nested messages to bundle buffer
                char* buf = (char*)malloc(len);
                inPacket->mSize = len;
                inPacket->mData = buf;

                memcpy(buf, inData, 16); // copy bundle header
                data = inData + 16; // skip bundle header
                while (data < dataEnd) {
                    int32 msgSize = OSCint(data);
                    data += sizeof(int32);
                    if (strcmp(data, "#bundle")) { // is a message
                        memcpy(buf, data - sizeof(int32), sizeof(int32) + msgSize);
                        buf += msgSize;
                    }
                    data += msgSize;
                }

                // process this packet without its nested bundle(s)
                if (!ProcessOSCPacket(inWorld, inPacket)) {
                    free(buf);
                    return false;
                }
            }

            // process nested bundle(s)
            data = inData + 16; // skip bundle header
            while (data < dataEnd) {
                int32 msgSize = OSCint(data);
                data += sizeof(int32);
                if (!strcmp(data, "#bundle")) { // is a bundle
                    OSC_Packet* packet = (OSC_Packet*)malloc(sizeof(OSC_Packet));
                    memcpy(packet, inPacket, sizeof(OSC_Packet)); // clone inPacket

                    if (!UnrollOSCPacket(inWorld, msgSize, data, packet)) {
                        free(packet);
                        return false;
                    }
                }
                data += msgSize;
            }
        } else { // !hasNestedBundle
            char* buf = (char*)malloc(inSize);
            inPacket->mSize = inSize;
            inPacket->mData = buf;
            memcpy(buf, inData, inSize);

            if (!ProcessOSCPacket(inWorld, inPacket)) {
                free(buf);
                return false;
            }
        }
    } else { // is a message
        char* buf = (char*)malloc(inSize);
        inPacket->mSize = inSize;
        inPacket->mData = buf;
        memcpy(buf, inData, inSize);

        if (!ProcessOSCPacket(inWorld, inPacket)) {
            free(buf);
            return false;
        }
    }

    return true;
}

} // namespace scsynth
