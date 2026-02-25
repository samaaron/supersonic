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


#include "SC_Lib_Cintf.h"
#include "SC_CoreAudio.h"
#include "SC_UnitDef.h"
#include "SC_BufGen.h"
#include "SC_World.h"
#include "SC_WorldOptions.h"
#include "SC_InterfaceTable.h"
#include <stdexcept>

// SUPERSONIC NOTE: Dynamic plugin loading code has been removed.
// SuperSonic compiles with -DSTATIC_PLUGINS — all UGen plugins are statically
// linked and loaded via direct function calls in initialize_library().
// The upstream SC_Lib_Cintf.cpp contains additional code for:
//   - PlugIn_Load() / PlugIn_LoadDir() — dynamic loading via dlopen/LoadLibrary
//   - checkAPIVersion() / checkServerVersion() — runtime API version checks
//   - Directory scanning for .scx plugin files
//   - Apple mach-o section preloading to avoid page-fault glitches
// None of this applies to WASM AudioWorklet builds.
// See upstream: server/scsynth/SC_Lib_Cintf.cpp

// From audio_processor.cpp
extern "C" {
    int worklet_debug(const char* fmt, ...);
}

Malloc gMalloc;
HashTable<SC_LibCmd, Malloc>* gCmdLib;
HashTable<struct UnitDef, Malloc>* gUnitDefLib = nullptr;
HashTable<struct BufGen, Malloc>* gBufGenLib = nullptr;
HashTable<struct PlugInCmd, Malloc>* gPlugInCmds = nullptr;
extern struct InterfaceTable gInterfaceTable;
SC_LibCmd* gCmdArray[NUMBER_OF_COMMANDS];

void initMiscCommands();

#ifdef STATIC_PLUGINS
// Plugin loader function declarations - these are defined in the plugin files
extern "C" {
    void IO_Load(InterfaceTable* table);
    void Osc_Load(InterfaceTable* table);
    void Delay_Load(InterfaceTable* table);
    void BinaryOp_Load(InterfaceTable* table);
    void Filter_Load(InterfaceTable* table);
    void Gendyn_Load(InterfaceTable* table);
    void LF_Load(InterfaceTable* table);
    void Noise_Load(InterfaceTable* table);
    void MulAdd_Load(InterfaceTable* table);
    void Grain_Load(InterfaceTable* table);
    void Pan_Load(InterfaceTable* table);
    void Reverb_Load(InterfaceTable* table);
    void Trigger_Load(InterfaceTable* table);
    void UnaryOp_Load(InterfaceTable* table);
#    ifndef NO_LIBSNDFILE
    void DiskIO_Load(InterfaceTable* table);
#    endif
    void Test_Load(InterfaceTable* table);
    void PhysicalModeling_Load(InterfaceTable* table);
    void Demand_Load(InterfaceTable* table);
    void DynNoise_Load(InterfaceTable* table);
    void FFT_UGens_Load(InterfaceTable* table);
    // void iPhone_Load(InterfaceTable* table);      // Not needed for WASM
    // sc3-plugins
    void Distortion_Load(InterfaceTable* table);
    void Mda_Load(InterfaceTable* table);
}

extern void DiskIO_Unload(void);
extern void UIUGens_Unload(void);
#endif // STATIC_PLUGINS

void deinitialize_library() {
#if defined(STATIC_PLUGINS) && !defined(NO_LIBSNDFILE)
    DiskIO_Unload();
    UIUGens_Unload();
#endif // STATIC_PLUGINS
}

void initialize_library(const char* uGensPluginPath) {
    gCmdLib = new HashTable<SC_LibCmd, Malloc>(&gMalloc, 64, true);
    gUnitDefLib = new HashTable<UnitDef, Malloc>(&gMalloc, 512, true);
    gBufGenLib = new HashTable<BufGen, Malloc>(&gMalloc, 512, true);
    gPlugInCmds = new HashTable<PlugInCmd, Malloc>(&gMalloc, 64, true);

    initMiscCommands();

#ifdef STATIC_PLUGINS
    IO_Load(&gInterfaceTable);
    Osc_Load(&gInterfaceTable);
    Delay_Load(&gInterfaceTable);
    BinaryOp_Load(&gInterfaceTable);
    Filter_Load(&gInterfaceTable);
    Gendyn_Load(&gInterfaceTable);
    LF_Load(&gInterfaceTable);
    Noise_Load(&gInterfaceTable);
    MulAdd_Load(&gInterfaceTable);
    Grain_Load(&gInterfaceTable);
    Pan_Load(&gInterfaceTable);
    Reverb_Load(&gInterfaceTable);
    Trigger_Load(&gInterfaceTable);
    UnaryOp_Load(&gInterfaceTable);
#    ifndef NO_LIBSNDFILE
    DiskIO_Load(&gInterfaceTable);
#    endif
    PhysicalModeling_Load(&gInterfaceTable);
    Test_Load(&gInterfaceTable);
    Demand_Load(&gInterfaceTable);
    DynNoise_Load(&gInterfaceTable);
#    if defined(SC_IPHONE) && !TARGET_IPHONE_SIMULATOR
    // iPhone_Load(&gInterfaceTable);  // Not needed for WASM
#    endif
    FFT_UGens_Load(&gInterfaceTable);
    // sc3-plugins
    Distortion_Load(&gInterfaceTable);
    Mda_Load(&gInterfaceTable);
#endif // STATIC_PLUGINS
}
