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

// TAU NOTE: Dynamic plugin loading code has been removed.
// SuperSonic compiles with -DSTATIC_PLUGINS — all UGen plugins are statically
// linked and loaded via direct function calls in initialize_library().
// The upstream SC_Lib_Cintf.cpp contains additional code for:
//   - PlugIn_Load() / PlugIn_LoadDir() — dynamic loading via dlopen/LoadLibrary
//   - checkAPIVersion() / checkServerVersion() — runtime API version checks
//   - Directory scanning for .scx plugin files
//   - Apple mach-o section preloading to avoid page-fault glitches
// None of this applies to SuperSonic builds (both WASM and native use STATIC_PLUGINS).
// See upstream: server/synth/SC_Lib_Cintf.cpp

#include "SC_HiddenWorld.h"   // the World's registries (SuperSonic, plural guest)
#include "SC_UnitDef.h"
#include "SC_BufGen.h"

void initMiscCommands(World* inWorld);

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
// demo plugins (for testing plugin/unit commands)
// Note: NOT extern "C" - must match C++ linkage of PluginLoad() macro
void DemoUGens_Load(InterfaceTable* table);
// Session-clock UGens (TempoBPM / TempoPhase). C++ linkage to match the
// PluginLoad() macro. Registered on EVERY build now: they read the clock
// mirror tau publishes, so there is no longer a "no-Link build" in
// which they would have nothing to report.
void LinkUGen_Load(InterfaceTable* table);
// SPLimiter: look-ahead limiter for the master bus. C++ linkage to
// match the PluginLoad() macro.
void SPLimiterUGen_Load(InterfaceTable* table);

extern void DiskIO_Unload();
extern void UIUGens_Unload();
#endif // STATIC_PLUGINS

// SuperSonic (plural guest): every entry of a World's registry, given back to the World's heap, then the
// table; a unit definition's own commands first.
void deinitialize_library(World* inWorld) {
#if defined(STATIC_PLUGINS) && !defined(NO_LIBSNDFILE)
    DiskIO_Unload();
    UIUGens_Unload();
#endif // STATIC_PLUGINS
    HiddenWorld* hw = inWorld->hw;
    if (hw->mUnitDefLib) {
        for (int32 i = 0; i < hw->mUnitDefLib->TableSize(); ++i) {
            UnitDef* def = hw->mUnitDefLib->AtIndex(i);
            if (!def) continue;
            if (def->mCmds) {
                for (int32 c = 0; c < def->mCmds->TableSize(); ++c) guest_delete(inWorld, def->mCmds->AtIndex(c));
                guest_delete(inWorld, def->mCmds);
            }
            guest_delete(inWorld, def);
        }
        guest_delete(inWorld, hw->mUnitDefLib);
        hw->mUnitDefLib = nullptr;
    }
    if (hw->mBufGenLib) {
        for (int32 i = 0; i < hw->mBufGenLib->TableSize(); ++i) guest_delete(inWorld, hw->mBufGenLib->AtIndex(i));
        guest_delete(inWorld, hw->mBufGenLib);
        hw->mBufGenLib = nullptr;
    }
    if (hw->mPlugInCmds) {
        for (int32 i = 0; i < hw->mPlugInCmds->TableSize(); ++i) guest_delete(inWorld, hw->mPlugInCmds->AtIndex(i));
        guest_delete(inWorld, hw->mPlugInCmds);
        hw->mPlugInCmds = nullptr;
    }
    if (hw->mCmdLib) {
        for (int32 i = 0; i < hw->mCmdLib->TableSize(); ++i) guest_delete(inWorld, hw->mCmdLib->AtIndex(i));
        guest_delete(inWorld, hw->mCmdLib);
        hw->mCmdLib = nullptr;
    }
    for (SC_LibCmd*& cmd : hw->mCmdArray) cmd = nullptr;
}

// SuperSonic (plural guest): the World's registries, from its heap, filled by the plugins' loads through
// the World's own copy of the interface table (which names the World).
void initialize_library(World* inWorld) {
    HiddenWorld* hw = inWorld->hw;
    GuestMalloc* heap = &hw->mGuestMalloc;
    hw->mCmdLib = guest_new<HashTable<SC_LibCmd, GuestMalloc>>(inWorld, heap, 64, true);
    hw->mUnitDefLib = guest_new<HashTable<UnitDef, GuestMalloc>>(inWorld, heap, 512, true);
    hw->mBufGenLib = guest_new<HashTable<BufGen, GuestMalloc>>(inWorld, heap, 512, true);
    hw->mPlugInCmds = guest_new<HashTable<PlugInCmd, GuestMalloc>>(inWorld, heap, 64, true);

    initMiscCommands(inWorld);

#ifdef STATIC_PLUGINS
    InterfaceTable* table = &hw->mInterfaceTable;
    IO_Load(table);
    Osc_Load(table);
    Delay_Load(table);
    BinaryOp_Load(table);
    Filter_Load(table);
    Gendyn_Load(table);
    LF_Load(table);
    Noise_Load(table);
    MulAdd_Load(table);
    Grain_Load(table);
    Pan_Load(table);
    Reverb_Load(table);
    Trigger_Load(table);
    UnaryOp_Load(table);
#    ifndef NO_LIBSNDFILE
    DiskIO_Load(table);
#    endif
    PhysicalModeling_Load(table);
    Test_Load(table);
    Demand_Load(table);
    DynNoise_Load(table);
#    if defined(SC_IPHONE) && !TARGET_IPHONE_SIMULATOR
    // iPhone_Load(table);  // Not needed for WASM
#    endif
    FFT_UGens_Load(table);
    // sc3-plugins
    Distortion_Load(table);
    Mda_Load(table);
    // demo plugins (for testing plugin/unit commands)
    DemoUGens_Load(table);
    LinkUGen_Load(table);
    SPLimiterUGen_Load(table);
#endif // STATIC_PLUGINS
}
