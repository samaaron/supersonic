/*
    SuperCollider real time audio synthesis system
    Copyright (c) 2002 James McCartney. All rights reserved.
    Copyright (c) 2013 Tim Blechmann.
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

#include <cstdint>

#include "SC_Reply.h"

enum Protocol {
    kUDP,
    kTCP
#ifdef SUPERSONIC
    , kWeb
#endif
};

struct ReplyAddress {
    // SuperSonic identifies the reply target by the origin token in mReplyData
    // (osc_reply_to_ring_buffer / the transport route by it); scsynth's network
    // address is unused, so this is a trivially-copyable placeholder on every
    // build — no boost::asio, on native or lean.
    uint32_t mAddressPlaceholder[4];
    enum Protocol mProtocol;
    int mPort;
    int mSocket;

    ReplyFunc mReplyFunc;
    void* mReplyData;
};

void null_reply_func(struct ReplyAddress* addr, char* msg, int size);

bool operator==(const ReplyAddress& a, const ReplyAddress& b);

bool operator<(const ReplyAddress& a, const ReplyAddress& b);

inline void SendReply(struct ReplyAddress* inReplyAddr, char* inBuf, int inSize) {
    (inReplyAddr->mReplyFunc)(inReplyAddr, inBuf, inSize);
}
