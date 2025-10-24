/*
 *    SuperCollider real time audio synthesis system
 *    Copyright (c) 2002 James McCartney. All rights reserved.
 *    Copyright (c) 2013 Tim Blechmann
 *
 *    This program is free software; you can redistribute it and/or modify
 *    it under the terms of the GNU General Public License as published by
 *    the Free Software Foundation; either version 2 of the License, or
 *    (at your option) any later version.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU General Public License for more details.
 *
 *    You should have received a copy of the GNU General Public License
 *    along with this program; if not, write to the Free Software
 *    Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301  USA
 */

#include <string>
#include <sstream>

static const int SC_VersionMajor = 3;
static const int SC_VersionMinor = 14;
static const int SC_VersionPatch = 0;
static const char SC_VersionTweak[] = "";
static const char SC_RefType[] = "branch";
static const char SC_BranchOrTag[] = "wasm-3-14";
static const char SC_CommitHash[] = "8a3bc4f";

// For backward compatibility in scsynth and supernova only.
static const char SC_VersionPostfix[] = ".0";

static inline std::string SC_VersionString()
{
	std::stringstream out;
	out << "scsynth-nrt " << SC_VersionMajor << "." << SC_VersionMinor << "." << SC_VersionPatch << SC_VersionTweak;
	return out.str();
}

static inline std::string SC_BuildString()
{
    std::stringstream out;
    out << "Built from " << SC_RefType << " '" << SC_BranchOrTag << "' [" << SC_CommitHash << "]";
    return out.str();
}
