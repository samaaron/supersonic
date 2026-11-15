/*
    SuperCollider real time audio synthesis system
    Copyright (c) 2002 James McCartney. All rights reserved.
    Copyright (c) 2012 Tim Blechmann
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

#include "SC_Reply.h"
#include "SC_ReplyImpl.hpp"
#include <cstring>

void null_reply_func(struct ReplyAddress* addr, char* msg, int size) {}

// mReplyData carries the token a message was sent with, and is the only field
// that differs between two SuperSonic reply addresses in one World: the
// network address upstream compared is a zeroed placeholder here. It is part
// of the comparison so that two addresses with different tokens are two
// addresses. hw->mUsers holds the client without a token (NotifyCmd::Stage2),
// so it has one entry however many times, and with whatever tokens, the
// client asks.
bool operator==(const ReplyAddress& a, const ReplyAddress& b) {
    return a.mWorld == b.mWorld
        && a.mProtocol == b.mProtocol && a.mPort == b.mPort && a.mSocket == b.mSocket
        && a.mReplyData == b.mReplyData;
}

bool operator<(const ReplyAddress& a, const ReplyAddress& b) {
    if (a.mWorld != b.mWorld) {
        return a.mWorld < b.mWorld;
    }
    if (a.mPort != b.mPort) {
        return a.mPort < b.mPort;
    } else if (a.mSocket != b.mSocket) {
        return a.mSocket < b.mSocket;
    } else if (a.mProtocol != b.mProtocol) {
        return a.mProtocol < b.mProtocol;
    } else {
        // Keeps the strict weak ordering consistent with operator== above.
        return a.mReplyData < b.mReplyData;
    }
}
