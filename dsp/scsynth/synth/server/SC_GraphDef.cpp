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


#ifdef _WIN32
#    include "SC_Win32Utils.h"
#endif

#include "clz.h"
#include "SC_Graph.h"
#include "SC_GraphDef.h"
#include "SC_Wire.h"
#include "SC_WireSpec.h"
#include "SC_UnitSpec.h"
#include "SC_UnitDef.h"
#include "SC_HiddenWorld.h"
#include "SC_WorldOptions.h"
#include "ReadWriteMacros.h"
#include "SC_Prototypes.h"
#include "SC_CoreAudio.h"
#include "SC_Errors.h"
#include "SC_Filesystem.hpp"
#include "buffer_commands.h"   // supersonic_guest_alloc_bytes: the guest's heap

#include <cassert>
#include <stdio.h>
#include <stdlib.h>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>
#include <memory>

#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

// =============================================================================
// TAU MODIFICATIONS
// =============================================================================
// This file has the following changes from upstream SuperCollider:
//
// 1. scprintf: Replaces scprintf throughout (declaration from SC_InterfaceTable.h)
// 2. Filesystem functions guarded with #ifndef __EMSCRIPTEN__ (GraphDef_Load,
//    GraphDef_LoadDir, GraphDef_LoadGlob, load_file — not available in WASM)
// 3. GraphDef_Recv: Extra std::string* outErrorMsg parameter for WASM error reporting
// 4. g_lastGraphDefError: Static error storage for Emscripten/WASM exception handling
//
// Backported from SuperCollider upstream commit 99be55460
// https://github.com/supercollider/supercollider/commit/99be55460
// Original PR: https://github.com/supercollider/supercollider/pull/7395
//
// Key changes from upstream:
// - SynthDef v3 format support (per-definition size field)
// - Unified GraphDef_Read/GraphDef_ReadVer1 into single version-aware reader
// - Bounds checking on all buffer reads (prevents crashes on truncated data)
// - malloc/calloc/free replaced with the guest's heap (guest_new_array and the rest, below)
// - unique_ptr for exception-safe GraphDef cleanup
// - BufColorAllocator modernized with std::vector (over the guest's heap)
// =============================================================================

const size_t ERR_BUF_SIZE(256);

// ── Where a synth definition is kept ─────────────────────────────────────────
// Upstream makes a definition's parts with new and frees them with delete,
// which is the system allocator. Here they come from the guest's heap
// (guest_heap.h: guest_new, guest_new_array and their frees, over
// supersonic_guest_alloc_bytes): a definition is read while sound plays, and
// clockwork's rule is that nothing on the audio thread reaches malloc. The
// heap being out is std::bad_alloc, as new would be: GraphDef_Recv catches
// it and the definition is refused whole.
//
// The parser's scratch too (GuestAllocator): the index map calcParamSpecs
// sorts and the two stacks of the buffer colouring were std::vectors on the
// system allocator — three mallocs on the audio thread for every definition
// loaded, measured by clockwork's census (2026-10-15), and the last ones
// this path made.

// Static error message storage for GraphDef parsing errors
// Used to preserve error messages across exception boundaries in Emscripten/WASM
// where C++ exception RTTI doesn't always work correctly
static std::string g_lastGraphDefError;

int32 GetHash(ParamSpec* inParamSpec) { return inParamSpec->mHash; }

int32* GetKey(ParamSpec* inParamSpec) { return inParamSpec->mName; }

// this is used for reading count fields that have been changed from int16 to int32 in SynthDef v2.
inline int32 readCount(const char*& buffer, const char* end, int version) {
    if (version >= 2)
        return readInt32_be(buffer, end);
    else
        return readInt16_be(buffer, end);
}

// A count is read before the array it sizes, and each element of the array
// takes at least `minBytes` of the def. A count the bytes that follow cannot
// hold is refused before anything is allocated from it: allocated first, it
// could exhaust the heap or, where size_t is 32 bits, wrap the array's size.
static uint32 checkArrayCount(int64 count, const char* buffer, const char* end, size_t minBytes, const char* what) {
    const size_t remaining = (size_t)(end - buffer);
    if (count >= 0 && (uint64)count <= remaining / minBytes)
        return (uint32)count;
    char str[ERR_BUF_SIZE];
    snprintf(str, ERR_BUF_SIZE, "SynthDef: %lld %s claimed, but only %zu bytes follow.", (long long)count, what,
             remaining);
    g_lastGraphDefError = str;
    throw std::runtime_error(str);
}

static uint32 readArrayCount(const char*& buffer, const char* end, int version, size_t minBytes, const char* what) {
    const int32 count = readCount(buffer, end, version);
    return checkArrayCount(count, buffer, end, minBytes, what);
}

// The fewest bytes a count field takes in a def of this version.
inline size_t countBytes(int version) { return version >= 2 ? 4 : 2; }

void ReadName(const char*& buffer, const char* end, int32* name) {
    uint32 namelen = readUInt8(buffer, end);
    if (namelen >= kSCNameByteLen) {
        std::ostringstream os;
        os << "name too long (> " << kSCNameByteLen - 1 << " chars): " << std::string(buffer, std::min<size_t>(namelen, end - buffer));
        throw std::runtime_error(os.str());
    }
    memset(name, 0, kSCNameByteLen);
    readData(buffer, end, (char*)name, namelen);
}

void ReadNodeDefName(const char*& buffer, const char* end, int32* name) {
    uint32 namelen = readUInt8(buffer, end);
    if (namelen >= kSCNodeDefNameByteLen) {
        std::ostringstream os;
        os << "node definition name too long (> " << kSCNodeDefNameByteLen - 1
           << " chars): " << std::string(buffer, std::min<size_t>(namelen, end - buffer));
        throw std::runtime_error(os.str());
    }
    memset(name, 0, kSCNodeDefNameByteLen);
    readData(buffer, end, (char*)name, namelen);
}

void ParamSpec_Read(ParamSpec* inParamSpec, const char*& buffer, const char* end, int version) {
    ReadName(buffer, end, inParamSpec->mName);
    inParamSpec->mIndex = readCount(buffer, end, version);
    inParamSpec->mHash = Hash(inParamSpec->mName);
}

void InputSpec_Read(InputSpec* inInputSpec, const char*& buffer, const char* end, int version) {
    inInputSpec->mFromUnitIndex = readCount(buffer, end, version);
    inInputSpec->mFromOutputIndex = readCount(buffer, end, version);

    inInputSpec->mWireIndex = -1;
}

void OutputSpec_Read(OutputSpec* inOutputSpec, const char*& buffer, const char* end) {
    inOutputSpec->mCalcRate = readInt8(buffer, end);
    inOutputSpec->mWireIndex = -1;
    inOutputSpec->mBufferIndex = -1;
    inOutputSpec->mNumConsumers = 0;
}

void UnitSpec_Read(UnitSpec* inUnitSpec, const char*& buffer, const char* end, int version) {
    int32 name[kSCNameLen];
    ReadName(buffer, end, name);

    inUnitSpec->mUnitDef = GetUnitDef(name);
    if (!inUnitSpec->mUnitDef) {
        char str[ERR_BUF_SIZE];
        snprintf(str, ERR_BUF_SIZE, "UGen '%s' not installed.", (char*)name);
        scprintf("ERROR: UGen '%s' not installed", (char*)name);
        g_lastGraphDefError = str;
        throw std::runtime_error(str);
    }
    inUnitSpec->mCalcRate = readInt8(buffer, end);

    const int32 numInputs = readCount(buffer, end, version);
    const int32 numOutputs = readCount(buffer, end, version);
    inUnitSpec->mSpecialIndex = readInt16_be(buffer, end);
    // Each input is two counts and each output its rate, one byte; the inputs
    // come first.
    const size_t inputBytes = 2 * countBytes(version);
    inUnitSpec->mNumInputs = checkArrayCount(numInputs, buffer, end, inputBytes, "unit inputs");
    inUnitSpec->mNumOutputs =
        checkArrayCount(numOutputs, buffer + inputBytes * inUnitSpec->mNumInputs, end, 1, "unit outputs");
    inUnitSpec->mInputSpec = guest_new_array<InputSpec>(inUnitSpec->mNumInputs);
    inUnitSpec->mOutputSpec = guest_new_array<OutputSpec>(inUnitSpec->mNumOutputs);
    for (uint32 i = 0; i < inUnitSpec->mNumInputs; ++i) {
        InputSpec_Read(inUnitSpec->mInputSpec + i, buffer, end, version);
    }
    for (uint32 i = 0; i < inUnitSpec->mNumOutputs; ++i) {
        OutputSpec_Read(inUnitSpec->mOutputSpec + i, buffer, end);
    }
    uint64 numPorts = inUnitSpec->mNumInputs + inUnitSpec->mNumOutputs;
    // Pointer-align so consecutive Units in the bump-allocated graph block
    // stay aligned (see SC_UnitDef.cpp).
    size_t raw = inUnitSpec->mUnitDef->mAllocSize + numPorts * (sizeof(Wire*) + sizeof(float*));
    inUnitSpec->mAllocSize = sc_align_up(raw, alignof(void*));
}

GraphDef* GraphDef_Read(World* inWorld, const char*& buffer, const char* end, GraphDef* inList, int32 inVersion);

GraphDef* GraphDefLib_Read(World* inWorld, const char* buffer, size_t size, GraphDef* inList) {
    const char* end = buffer + size;

    // check header ('SCgf')
    int32 magic = readInt32_be(buffer, end);
    if (magic != (('S' << 24) | ('C' << 16) | ('g' << 8) | 'f'))
        throw std::runtime_error("not a synthdef");

    int32 version = readInt32_be(buffer, end);
    if (version > 3)
        throw std::runtime_error("version " + std::to_string(version) + " not supported");

    uint32 numDefs = readInt16_be(buffer, end);
    if (version > 2) {
        // in version 3, every synth definition starts with a size field (int32) that tells
        // the size of the entire definition in bytes (including the size field itself).
        // NB: GraphDef_Read() might not read all the fields so we must explicitly set
        // the begin and end on each iteration!
        for (int i = 0; i < numDefs; ++i) {
            // The size counts its own four bytes; the def must lie inside
            // what was sent, which is checked before its end is worked out.
            const int32 synthDefSize = readInt32_be(buffer, end);
            if (synthDefSize < 4 || (size_t)(synthDefSize - 4) > (size_t)(end - buffer))
                throw std::runtime_error("wrong synthdef size");
            const char* synthDefEnd = buffer + (synthDefSize - 4);

            inList = GraphDef_Read(inWorld, buffer, synthDefEnd, inList, version);

            buffer = synthDefEnd;
        }
    } else {
        for (int i = 0; i < numDefs; ++i) {
            inList = GraphDef_Read(inWorld, buffer, end, inList, version);
        }
    }

    return inList;
}


void ChooseMulAddFunc(GraphDef* graphDef, UnitSpec* unitSpec);
void DoBufferColoring(World* inWorld, GraphDef* inGraphDef);

void GraphDef_ReadVariant(World* inWorld, const char*& buffer, const char* end, GraphDef* inGraphDef,
                          GraphDef* inVariant) {
    memcpy(inVariant, inGraphDef, sizeof(GraphDef));

    inVariant->mNumVariants = 0;
    inVariant->mVariants = nullptr;

    ReadName(buffer, end, inVariant->mNodeDef.mName);
    inVariant->mNodeDef.mHash = Hash(inVariant->mNodeDef.mName);

    inVariant->mInitialControlValues = guest_new_array<float32>(inGraphDef->mNumControls);
    for (uint32 i = 0; i < inGraphDef->mNumControls; ++i) {
        inVariant->mInitialControlValues[i] = readFloat_be(buffer, end);
    }
}


typedef struct IndexMap {
    uint32 index;
    uint32 paramSpecIndex;
} IndexMap;

inline static void calcParamSpecs(GraphDef* graphDef, const char*& buffer, const char* end, int version) {
    uint32 numSpecs = graphDef->mNumParamSpecs;
    if (numSpecs > 0) {
        int hashTableSize = NEXTPOWEROFTWO(numSpecs);
        graphDef->mParamSpecTable = guest_new<ParamSpecTable>(&gGuestMalloc, hashTableSize, false);
        graphDef->mParamSpecs = guest_new_array<ParamSpec>(numSpecs);
        std::vector<IndexMap, GuestAllocator<IndexMap>> tempMaps(numSpecs);

        for (uint32 i = 0; i < numSpecs; ++i) {
            ParamSpec* paramSpec = &graphDef->mParamSpecs[i];
            ParamSpec_Read(paramSpec, buffer, end, version);
            graphDef->mParamSpecTable->Add(paramSpec);
            tempMaps[i].index = i;
            tempMaps[i].paramSpecIndex = paramSpec->mIndex;
        }
        // calculate numChannels for each spec
        std::sort(tempMaps.begin(), tempMaps.end(),
                  [](const auto& a, const auto& b) { return a.paramSpecIndex < b.paramSpecIndex; });
        for (uint32 i = 0; i < (numSpecs - 1); ++i) {
            const auto& tempMap = tempMaps[i];
            const auto& nextTempMap = tempMaps[i + 1];
            auto& paramSpec = graphDef->mParamSpecs[tempMap.index];
            paramSpec.mNumChannels = nextTempMap.paramSpecIndex - tempMap.paramSpecIndex;
        }

        // last spec
        const auto& tempMap = tempMaps.back();
        auto& paramSpec = graphDef->mParamSpecs[tempMap.index];
        paramSpec.mNumChannels = graphDef->mNumControls - tempMap.paramSpecIndex;
    } else {
        // empty table to eliminate test in Graph_SetControl
        graphDef->mParamSpecTable = guest_new<ParamSpecTable>(&gGuestMalloc, 4, false);
        graphDef->mParamSpecs = nullptr;
    }
}

// The GraphDef block is bump-allocated as a sequence of sub-arrays (Wires,
// Unit*, controls, float*, ints, …). Each sub-array's start must be at
// least pointer-aligned: a section of N×4-byte ints would otherwise leave
// the bump pointer 4-aligned, misaligning the next section's 8-byte
// pointers.
static void GraphDef_SetAllocSizes(GraphDef* graphDef) {
    constexpr size_t a = alignof(void*);
    graphDef->mWiresAllocSize           = sc_align_up(graphDef->mNumWires     * sizeof(Wire),    a);
    graphDef->mUnitsAllocSize           = sc_align_up(graphDef->mNumUnitSpecs * sizeof(Unit*),   a);
    graphDef->mCalcUnitsAllocSize       = sc_align_up(graphDef->mNumCalcUnits * sizeof(Unit*),   a);
    graphDef->mControlAllocSize         = sc_align_up(graphDef->mNumControls  * sizeof(float),   a);
    graphDef->mMapControlsAllocSize     = sc_align_up(graphDef->mNumControls  * sizeof(float*),  a);
    graphDef->mMapControlRatesAllocSize = sc_align_up(graphDef->mNumControls  * sizeof(int),     a);
    graphDef->mAudioMapBusOffsetSize    = sc_align_up(graphDef->mNumControls  * sizeof(int32),   a);

    graphDef->mNodeDef.mAllocSize += graphDef->mWiresAllocSize;
    graphDef->mNodeDef.mAllocSize += graphDef->mUnitsAllocSize;
    graphDef->mNodeDef.mAllocSize += graphDef->mCalcUnitsAllocSize;
    graphDef->mNodeDef.mAllocSize += graphDef->mControlAllocSize;
    graphDef->mNodeDef.mAllocSize += graphDef->mMapControlsAllocSize;
    graphDef->mNodeDef.mAllocSize += graphDef->mMapControlRatesAllocSize;
    graphDef->mNodeDef.mAllocSize += graphDef->mAudioMapBusOffsetSize;
}


// Every input must name a constant that exists or an output of an earlier
// unit; a negative unit index means a constant. Buffer colouring and graph
// construction index arrays with these numbers unchecked.
static void CheckInputSpecs(const GraphDef* graphDef) {
    for (uint32 j = 0; j < graphDef->mNumUnitSpecs; ++j) {
        const UnitSpec* unitSpec = graphDef->mUnitSpecs + j;
        for (uint32 i = 0; i < unitSpec->mNumInputs; ++i) {
            const InputSpec* in = unitSpec->mInputSpec + i;
            const int32 unit = in->mFromUnitIndex;
            const int32 index = in->mFromOutputIndex;
            const bool exists = unit < 0
                ? index >= 0 && uint32(index) < graphDef->mNumConstants
                : uint32(unit) < j && index >= 0 && uint32(index) < graphDef->mUnitSpecs[unit].mNumOutputs;
            if (exists)
                continue;
            char str[ERR_BUF_SIZE];
            if (unit < 0)
                snprintf(str, ERR_BUF_SIZE, "SynthDef '%s': unit %u input %u reads constant %d, which does not exist.",
                         (const char*)graphDef->mNodeDef.mName, j, i, index);
            else
                snprintf(str, ERR_BUF_SIZE,
                         "SynthDef '%s': unit %u input %u reads output %d of unit %d, which is not an earlier unit's output.",
                         (const char*)graphDef->mNodeDef.mName, j, i, index, unit);
            g_lastGraphDefError = str;
            throw std::runtime_error(str);
        }
    }
}

/** \note Relevant supernova code: \c sc_synthdef::sc_synthdef() */
GraphDef* GraphDef_Read(World* inWorld, const char*& buffer, const char* end, GraphDef* inList, int32 inVersion) {
    int32 name[kSCNodeDefNameLen];
    ReadNodeDefName(buffer, end, name);

    // Use custom deleter for automatic cleanup in case an exception is thrown.
    auto deleter = [](GraphDef* def) { GraphDef_Free(def); };
    // value-initialize so that all members of GraphDef are initially set to zero!
    std::unique_ptr<GraphDef, decltype(deleter)> graphDef(guest_new<GraphDef>(), deleter);

    graphDef->mOriginal = graphDef.get();

    graphDef->mNodeDef.mAllocSize = sizeof(Graph);

    memcpy(graphDef->mNodeDef.mName, name, kSCNodeDefNameByteLen);

    graphDef->mNodeDef.mHash = Hash(graphDef->mNodeDef.mName);

    graphDef->mNumConstants = readArrayCount(buffer, end, inVersion, sizeof(float32), "constants");
    graphDef->mConstants = guest_new_array<float32>(graphDef->mNumConstants);
    for (uint32 i = 0; i < graphDef->mNumConstants; ++i) {
        graphDef->mConstants[i] = readFloat_be(buffer, end);
    }
    graphDef->mNumWires = graphDef->mNumConstants;

    graphDef->mNumControls = readArrayCount(buffer, end, inVersion, sizeof(float32), "controls");
    graphDef->mInitialControlValues = guest_new_array<float32>(graphDef->mNumControls);
    for (uint32 i = 0; i < graphDef->mNumControls; ++i) {
        graphDef->mInitialControlValues[i] = readFloat_be(buffer, end);
    }

    // A control name is its length byte and its control index at the least.
    graphDef->mNumParamSpecs = readArrayCount(buffer, end, inVersion, 1 + countBytes(inVersion), "control names");
    calcParamSpecs(graphDef.get(), buffer, end, inVersion);

    // A unit is its name's length byte, its rate, two counts and its special index at the least.
    uint32 numUnitSpecs = readArrayCount(buffer, end, inVersion, 1 + 1 + 2 * countBytes(inVersion) + 2, "units");
    graphDef->mUnitSpecs = guest_new_array<UnitSpec>(numUnitSpecs);
    graphDef->mNumCalcUnits = 0;
    graphDef->mNumUnitSpecs = 0;
    for (uint32 i = 0; i < numUnitSpecs; ++i) {
        UnitSpec* unitSpec = &graphDef->mUnitSpecs[i];
        UnitSpec_Read(unitSpec, buffer, end, inVersion);

        switch (unitSpec->mCalcRate) {
        case calc_ScalarRate:
            unitSpec->mRateInfo = &inWorld->mBufRate;
            break;
        case calc_BufRate:
            graphDef->mNumCalcUnits++;
            unitSpec->mRateInfo = &inWorld->mBufRate;
            break;
        case calc_FullRate:
            graphDef->mNumCalcUnits++;
            unitSpec->mRateInfo = &inWorld->mFullRate;
            break;
        case calc_DemandRate:
            unitSpec->mRateInfo = &inWorld->mBufRate;
            break;
        }

        // increment mNumUnitSpecs after every successful call to UnitSpec_Read() so that we
        // can safely call GraphDef_Free() when we catch an exception.
        graphDef->mNumUnitSpecs++;
        graphDef->mNodeDef.mAllocSize += unitSpec->mAllocSize;
        graphDef->mNumWires += unitSpec->mNumOutputs;
    }

    CheckInputSpecs(graphDef.get());
    DoBufferColoring(inWorld, graphDef.get());

    GraphDef_SetAllocSizes(graphDef.get());

    if (inVersion >= 1) {
        // A variant is its name's length byte and a value for every control at the least.
        const uint16 claimedVariants = readInt16_be(buffer, end);
        uint32 numVariants =
            checkArrayCount(claimedVariants, buffer, end, 1 + sizeof(float32) * graphDef->mNumControls, "variants");
        if (numVariants > 0) {
            graphDef->mVariants = guest_new_array<GraphDef>(numVariants);
            for (uint32 i = 0; i < numVariants; ++i) {
                GraphDef_ReadVariant(inWorld, buffer, end, graphDef.get(), graphDef->mVariants + i);
                // increment mNumVariants after every successful call to GraphDef_ReadVariant()
                // so that we can safely call GraphDef_Free() when we catch an exception.
                graphDef->mNumVariants++;
            }
        }
    }

    if (inVersion > 2) {
        // reblock
        graphDef->mBlockSize = readInt32_be(buffer, end);
        graphDef->mBlockSizeIndex = readInt32_be(buffer, end);
        // resample
        graphDef->mResampleFactor = readFloat_be(buffer, end);
        graphDef->mResampleIndex = readInt32_be(buffer, end);
    } else {
        graphDef->mBlockSize = 0;
        graphDef->mBlockSizeIndex = 0;
        graphDef->mResampleFactor = 1.0;
        graphDef->mResampleIndex = 0;
    }

    // A unit that reads a whole block from an input reads a block on from the
    // input's buffer, and an input that is not audio rate holds one value in
    // its wire, inside the synth's own memory. A SynthDef can give any unit
    // any input, so the synth ends a block on from its last byte.
    const bool reblocks = graphDef->mBlockSize != 0 || graphDef->mResampleFactor != 1.0f;
    const size_t maxBlock = reblocks ? (size_t)inWorld->hw->mMaxWireBufs * inWorld->mBufLength : inWorld->mBufLength;
    graphDef->mNodeDef.mAllocSize += sc_align_up(maxBlock * sizeof(float), alignof(void*));

    // Variants were copied before the size and the reblock settings were known.
    for (uint32 i = 0; i < graphDef->mNumVariants; ++i) {
        GraphDef* variant = graphDef->mVariants + i;
        variant->mNodeDef.mAllocSize = graphDef->mNodeDef.mAllocSize;
        variant->mBlockSize = graphDef->mBlockSize;
        variant->mBlockSizeIndex = graphDef->mBlockSizeIndex;
        variant->mResampleFactor = graphDef->mResampleFactor;
        variant->mResampleIndex = graphDef->mResampleIndex;
    }

    // finally add to list
    graphDef->mNext = inList;
    graphDef->mRefCount = 1;

    return graphDef.release();
}

void GraphDef_Define(World* inWorld, GraphDef* inList) {
    GraphDef* graphDef = inList;
    while (graphDef) {
        GraphDef* next = graphDef->mNext;

        GraphDef* previousDef = World_GetGraphDef(inWorld, graphDef->mNodeDef.mName);
        if (previousDef) {
            World_RemoveGraphDef(inWorld, previousDef);
            GraphDef_Release(inWorld, previousDef);
        }
        World_AddGraphDef(inWorld, graphDef);
        graphDef->mNext = nullptr;
        graphDef = next;
    }
}

/** @brief Remove a GraphDef.
 *  Removes the GraphDef from the GraphDefLib and decrements its reference count.
 *  Implements the \c /d_free command, see \c meth_d_free().
 */
SCErr GraphDef_Remove(World* inWorld, int32* inName) {
    GraphDef* graphDef = World_GetGraphDef(inWorld, inName);
    if (graphDef) {
        World_RemoveGraphDef(inWorld, graphDef);
        return GraphDef_Release(inWorld, graphDef);
    }
    return kSCErr_SynthDefNotFound;
}

/** @brief Release a GraphDef.
 *  Decrements the GraphDef's reference count.
 *  When the count reaches zero, the GraphDef is actually freed.
 */
SCErr GraphDef_Release(World* inWorld, GraphDef* inGraphDef) {
    assert(inGraphDef->mRefCount > 0);
    if (--inGraphDef->mRefCount == 0) {
        // Freed here, with clients told: see DeleteGraphDefMsg (SC_HiddenWorld.h).
        DeleteGraphDefMsg msg;
        msg.mWorld = inWorld;
        msg.mDefList = inGraphDef;
        msg.Perform();
    }
    return kSCErr_None;
}

GraphDef* GraphDef_Recv(World* inWorld, const char* buffer, size_t size, GraphDef* inList, std::string* outErrorMsg) {
    g_lastGraphDefError.clear();
    try {
        inList = GraphDefLib_Read(inWorld, buffer, size, inList);
    } catch (std::exception& exc) {
        scprintf("exception in GraphDef_Recv: %s\n", exc.what());
        if (outErrorMsg) {
            *outErrorMsg = exc.what();
        }
    } catch (...) {
        // Emscripten WASM exception handling may not preserve std::exception type info,
        // so use g_lastGraphDefError which was set before throwing
        if (!g_lastGraphDefError.empty()) {
            scprintf("exception in GraphDef_Recv: %s\n", g_lastGraphDefError.c_str());
            if (outErrorMsg) {
                *outErrorMsg = g_lastGraphDefError;
            }
        } else {
            scprintf("unknown exception in GraphDef_Recv\n");
            if (outErrorMsg) {
                *outErrorMsg = "unknown exception";
            }
        }
    }

    return inList;
}

#ifndef __EMSCRIPTEN__
GraphDef* GraphDef_LoadGlob(World* inWorld, const char* pattern, GraphDef* inList) {
    SC_Filesystem::Glob* glob = SC_Filesystem::makeGlob(pattern);
    if (!glob)
        return inList;

    fs::path path;
    while (!(path = SC_Filesystem::globNext(glob)).empty()) {
        if (path.extension() == ".scsyndef") {
            inList = GraphDef_Load(inWorld, path, inList);
        }
        // why? <sk>
        GraphDef_Load(inWorld, path, inList);
    }

    SC_Filesystem::freeGlob(glob);
    return inList;
}

std::string load_file(const std::filesystem::path& file_path) {
    std::ifstream file(file_path, std::ifstream::binary);
    if (!file.is_open()) {
        throw std::runtime_error("Could not open file: " + file_path.string());
    }
    std::stringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

GraphDef* GraphDef_Load(World* inWorld, const fs::path& path, GraphDef* inList) {
    try {
        std::string file_contents = load_file(path);
        inList = GraphDefLib_Read(inWorld, file_contents.data(), file_contents.size(), inList);
    } catch (const std::exception& e) {
        scprintf("exception in GraphDef_Load: %s\n", e.what());
    } catch (...) {
        scprintf("unknown exception in GraphDef_Load\n");
    }

    return inList;
}

GraphDef* GraphDef_LoadDir(World* inWorld, const fs::path& dirname, GraphDef* inList) {
    std::error_code ec;
    fs::recursive_directory_iterator rditer(dirname, fs::directory_options::follow_directory_symlink, ec);

    if (ec) {
        scprintf("*** ERROR: open directory failed '%s'\n", SC_Codecvt::path_to_utf8_str(dirname).c_str());
        return inList;
    }

    while (rditer != fs::end(rditer)) {
        const fs::path path = *rditer;

        if (fs::is_directory(path)) {
            if (SC_Filesystem::instance().shouldNotCompileDirectory(path))
                rditer.disable_recursion_pending();
            else
                ; // do nothing; recursion will happen automatically
        } else if (path.extension() == ".scsyndef") { // ordinary file
            inList = GraphDef_Load(inWorld, path, inList);
        } else {
            // ignore file, wasn't a synth def
        }

        rditer.increment(ec);
        if (ec) {
            scprintf("*** ERROR: Could not iterate on '%s': %s\n", SC_Codecvt::path_to_utf8_str(path).c_str(),
                     ec.message().c_str());
            return inList;
        }
    }

    return inList;
}
#endif // !__EMSCRIPTEN__

void UnitSpec_Free(UnitSpec* inUnitSpec) {
    guest_delete_array(inUnitSpec->mInputSpec);
    guest_delete_array(inUnitSpec->mOutputSpec);
}

void GraphDef_Free(GraphDef* inGraphDef) {
    if (inGraphDef != inGraphDef->mOriginal)
        return;

    for (uint32 i = 0; i < inGraphDef->mNumUnitSpecs; ++i) {
        UnitSpec_Free(inGraphDef->mUnitSpecs + i);
    }
    for (uint32 i = 0; i < inGraphDef->mNumVariants; ++i) {
        guest_delete_array(inGraphDef->mVariants[i].mInitialControlValues);
    }
    guest_delete(inGraphDef->mParamSpecTable);
    guest_delete_array(inGraphDef->mParamSpecs);
    guest_delete_array(inGraphDef->mInitialControlValues);
    guest_delete_array(inGraphDef->mConstants);
    guest_delete_array(inGraphDef->mUnitSpecs);
    guest_delete_array(inGraphDef->mVariants);
    guest_delete(inGraphDef);
}

void NodeDef_Dump(NodeDef* inNodeDef) {
    scprintf("mName '%s'\n", (char*)inNodeDef->mName);
    scprintf("mHash %d\n", inNodeDef->mHash);
    scprintf("mAllocSize %lu\n", inNodeDef->mAllocSize);
}

void GraphDef_Dump(GraphDef* inGraphDef) {
    NodeDef_Dump(&inGraphDef->mNodeDef);

    scprintf("mNumControls %d\n", inGraphDef->mNumControls);
    scprintf("mNumWires %d\n", inGraphDef->mNumWires);
    scprintf("mNumUnitSpecs %d\n", inGraphDef->mNumUnitSpecs);
    scprintf("mNumWireBufs %d\n", inGraphDef->mNumWireBufs);

    for (uint32 i = 0; i < inGraphDef->mNumControls; ++i) {
        scprintf("   %d mInitialControlValues %g\n", i, inGraphDef->mInitialControlValues[i]);
    }

    for (uint32 i = 0; i < inGraphDef->mNumWires; ++i) {
        // WireSpec_Dump(inGraphDef->mWireSpec + i);
    }
    for (uint32 i = 0; i < inGraphDef->mNumUnitSpecs; ++i) {
        // UnitSpec_Dump(inGraphDef->mUnitSpecs + i);
    }
}

class BufColorAllocator {
public:
    BufColorAllocator();

    uint32 alloc(uint32 count);
    bool release(int inIndex);
    int NumBufs() { return mRefs.size(); }

private:
    std::vector<int16, GuestAllocator<int16>> mRefs;
    std::vector<int16, GuestAllocator<int16>> mStack;
};

inline BufColorAllocator::BufColorAllocator() {
    mRefs.reserve(32);
    mStack.reserve(32);
}

inline uint32 BufColorAllocator::alloc(uint32 count) {
    uint32 outIndex;
    if (!mStack.empty()) {
        // pop index from stack
        outIndex = mStack.back();
        mStack.pop_back();
        mRefs[outIndex] = count;
    } else {
        // make new index
        outIndex = mRefs.size();
        mRefs.push_back(count);
    }
    return outIndex;
}

inline bool BufColorAllocator::release(int inIndex) {
    if (mRefs[inIndex] == 0)
        return false;
    if (--mRefs[inIndex] == 0) {
        mStack.push_back(inIndex);
    }
    return true;
}

static void ReleaseInputBuffers(GraphDef* inGraphDef, UnitSpec* unitSpec, BufColorAllocator& bufColor) {
    for (int64 i = (int64)(unitSpec->mNumInputs) - 1; i >= 0; --i) {
        InputSpec* inputSpec = unitSpec->mInputSpec + i;
        if (inputSpec->mFromUnitIndex >= 0) {
            UnitSpec* outUnit = inGraphDef->mUnitSpecs + inputSpec->mFromUnitIndex;
            OutputSpec* outputSpec = outUnit->mOutputSpec + inputSpec->mFromOutputIndex;
            inputSpec->mWireIndex = outputSpec->mWireIndex;
            if (outputSpec->mCalcRate == calc_FullRate) {
                if (unitSpec->mCalcRate == calc_DemandRate)
                    // we never release any input buffers of demand-rate ugens
                    continue;

                if (!bufColor.release(outputSpec->mBufferIndex)) {
                    throw std::runtime_error("buffer coloring error.");
                }
            }
        } else {
            inputSpec->mWireIndex = inputSpec->mFromOutputIndex;
        }
    }
}

static void AllocOutputBuffers(UnitSpec* unitSpec, BufColorAllocator& bufColor, int32& wireIndexCtr) {
    for (uint32 i = 0; i < unitSpec->mNumOutputs; ++i) {
        OutputSpec* outputSpec = unitSpec->mOutputSpec + i;
        outputSpec->mWireIndex = wireIndexCtr++;
        if (outputSpec->mCalcRate == calc_FullRate) {
            uint32 bufIndex = bufColor.alloc(outputSpec->mNumConsumers);
            outputSpec->mBufferIndex = bufIndex;
        }
    }
}

void DoBufferColoring(World* inWorld, GraphDef* inGraphDef) {
    // count consumers of outputs
    for (uint32 j = 0; j < inGraphDef->mNumUnitSpecs; ++j) {
        UnitSpec* unitSpec = inGraphDef->mUnitSpecs + j;
        for (uint32 i = 0; i < unitSpec->mNumInputs; ++i) {
            InputSpec* inputSpec = unitSpec->mInputSpec + i;
            if (inputSpec->mFromUnitIndex >= 0) {
                UnitSpec* outUnit = inGraphDef->mUnitSpecs + inputSpec->mFromUnitIndex;
                OutputSpec* outputSpec = outUnit->mOutputSpec + inputSpec->mFromOutputIndex;
                outputSpec->mNumConsumers++;
            }
        }
    }

    // buffer coloring
    {
        BufColorAllocator bufColor;
        int32 wireIndexCtr = inGraphDef->mNumConstants; // mNumConstants is a uint32, but limited to int32 in OSC
        for (uint32 j = 0; j < inGraphDef->mNumUnitSpecs; ++j) {
            UnitSpec* unitSpec = inGraphDef->mUnitSpecs + j;
            if (unitSpec->mUnitDef->mFlags & kUnitDef_CantAliasInputsToOutputs) {
                // set wire index, alloc outputs
                AllocOutputBuffers(unitSpec, bufColor, wireIndexCtr);
                // set wire index, release inputs
                ReleaseInputBuffers(inGraphDef, unitSpec, bufColor);
            } else {
                // set wire index, release inputs
                ReleaseInputBuffers(inGraphDef, unitSpec, bufColor);
                // set wire index, alloc outputs
                AllocOutputBuffers(unitSpec, bufColor, wireIndexCtr);
            }
        }

        inGraphDef->mNumWireBufs = bufColor.NumBufs();
        if (inWorld->mRunning) {
            // cannot reallocate interconnect buffers while running audio.
            if (inGraphDef->mNumWireBufs > inWorld->hw->mMaxWireBufs) {
                throw std::runtime_error("exceeded number of interconnect buffers.");
            }
        } else {
            inWorld->hw->mMaxWireBufs = sc_max(inWorld->hw->mMaxWireBufs, inGraphDef->mNumWireBufs);
        }
    }
}
