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

#include "SC_Lib.h"
#include "SC_Reply.h"
#include "SC_OSC_Commands.h"


typedef SCErr (*SC_CommandFunc)(struct World* inWorld, int inSize, const char* inData, ReplyAddress* inReply);

class SC_LibCmd : public SC_NamedObj {
public:
    SC_LibCmd(SC_CommandFunc inFunc);

    SCErr Perform(struct World* inWorld, int inSize, const char* inData, ReplyAddress* inReply);

private:
    SC_CommandFunc mFunc;
};

// SuperSonic (plural guest): the command library, the unit definitions, the
// buffer generators and the plugin commands are one set per World
// (HiddenWorld), built and freed with it, where upstream's are one set per
// process behind these globals.
#ifndef CLOCKWORK_GUEST
extern Malloc gMalloc;
extern HashTable<class SC_LibCmd, Malloc>* gCmdLib;
extern SC_LibCmd* gCmdArray[NUMBER_OF_COMMANDS];
#endif

void initialize_library(struct World* inWorld);
void deinitialize_library(struct World* inWorld);
SCErr NewCommand(struct World* inWorld, const char* inPath, uint32 inCommandNumber, SC_CommandFunc inFunc);
