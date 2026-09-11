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

// ReplyAddress carries a trivially-copyable placeholder (not a
// boost::asio::ip::address) on EVERY build: SuperSonic routes replies by the
// origin token in mReplyData, so scsynth's network address is unused and no
// build pulls boost::asio into the reply path.
#include "SC_Types.h"

struct ReplyAddress;
typedef void (*ReplyFunc)(struct ReplyAddress* inReplyAddr, char* inBuf, int inSize);
