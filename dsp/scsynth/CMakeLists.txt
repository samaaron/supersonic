# SPDX-License-Identifier: GPL-3.0-or-later
#
# The scsynth guest — target `scsynth`, which is what CLOCKWORK_DSP names.
#
# GPL-3.0-or-later. Everything under this directory is the engine. The
# clockwork it links is AGPL-3.0-or-later (or commercially licensed; see
# clockwork/LICENSE), so the combined binary is conveyed under the AGPL, the
# GPLv3 permitting that combination in its section 13. The boundary is
# dsp_api.h, one directory up in the submodule.

# ── The engine ───────────────────────────────────────────────────────────────
# scsynth's own three groups, carried over unchanged from upstream SuperSonic
# so the diff against it stays readable.
set(SCSYNTH_SERVER_SOURCES
    synth/server/SC_BufGen.cpp
    synth/server/SC_EngineCore.cpp
    synth/server/SC_Graph.cpp
    synth/server/SC_GraphDef.cpp
    synth/server/SC_Group.cpp
    synth/server/SC_Lib.cpp
    synth/server/SC_Lib_Cintf.cpp
    synth/server/SC_MiscCmds.cpp
    synth/server/SC_Node.cpp
    synth/server/SC_OscUnroll.cpp
    synth/server/SC_Rate.cpp
    synth/server/SC_SequencedCommand.cpp
    synth/server/SC_Str4.cpp
    synth/server/SC_Stubs.cpp
    synth/server/SC_Unit.cpp
    synth/server/SC_UnitDef.cpp
    synth/server/SC_World.cpp)

# The platform half of SC_Filesystem. scsynth still reaches for resource and
# config directories even though clockwork gives it neither.
if(WIN32)
    set(SCSYNTH_PLATFORM_SOURCES synth/common/SC_Filesystem_win.cpp)
elseif(APPLE)
    # SC_Filesystem_macos.cpp includes Foundation headers, so it is Objective-C++
    # despite the .cpp suffix; without this it is compiled as plain C++ and the
    # SDK headers fail on the first @class. SC_Apple.mm is its companion.
    set_source_files_properties(synth/common/SC_Filesystem_macos.cpp
        PROPERTIES LANGUAGE OBJCXX)
    set(SCSYNTH_PLATFORM_SOURCES
        synth/common/SC_Filesystem_macos.cpp
        synth/common/SC_Apple.mm)
else()
    set(SCSYNTH_PLATFORM_SOURCES synth/common/SC_Filesystem_unix.cpp)
endif()

set(SCSYNTH_COMMON_SOURCES
    synth/common/SC_AllocPool.cpp
    synth/common/SC_Errors.cpp
    synth/common/SC_Reply.cpp
    synth/common/SC_ServerBootDelayWarning.cpp
    synth/common/SC_StringBuffer.cpp
    synth/common/SC_StringParser.cpp
    synth/common/SC_TextUtils.cpp
    synth/common/SC_fftlib.cpp
    synth/common/Samp.cpp
    synth/common/fftlib.c)

set(SCSYNTH_PLUGIN_SOURCES
    synth/plugins/BinaryOpUGens.cpp
    synth/plugins/ChaosUGens.cpp
    synth/plugins/Convolution.cpp
    synth/plugins/DelayUGens.cpp
    synth/plugins/DemandUGens.cpp
    synth/plugins/DemoUGens.cpp
    synth/plugins/DynNoiseUGens.cpp
    synth/plugins/FFT2InterfaceTable.cpp
    synth/plugins/FFTInterfaceTable.cpp
    synth/plugins/FFT_UGens.cpp
    synth/plugins/FeatureDetection.cpp
    synth/plugins/FilterUGens.cpp
    synth/plugins/GendynUGens.cpp
    synth/plugins/GrainUGens.cpp
    synth/plugins/IOUGens.cpp
    synth/plugins/LFUGens.cpp
    synth/plugins/LinkUGen.cpp
    synth/plugins/MulAddUGens.cpp
    synth/plugins/NoiseUGens.cpp
    synth/plugins/OscUGens.cpp
    synth/plugins/SPLimiterUGen.cpp
    synth/plugins/PV_ThirdParty.cpp
    synth/plugins/PV_UGens.cpp
    synth/plugins/PanUGens.cpp
    synth/plugins/PartitionedConvolution.cpp
    synth/plugins/PhysicalModelingUGens.cpp
    synth/plugins/ReverbUGens.cpp
    synth/plugins/TestUGens.cpp
    synth/plugins/TriggerUGens.cpp
    synth/plugins/UnaryOpUGens.cpp
    synth/plugins/UnpackFFTUGens.cpp
    synth/plugins/sc3-plugins/DistortionUGens.cpp
    synth/plugins/sc3-plugins/MdaUGens.cpp)

add_library(scsynth STATIC
    scsynth_dsp.cpp
    buffer_commands.cpp
    piano_wavetable.cpp
    node_tree.cpp
    ${SCSYNTH_SERVER_SOURCES}
    ${SCSYNTH_COMMON_SOURCES}
    ${SCSYNTH_PLATFORM_SOURCES}
    ${SCSYNTH_PLUGIN_SOURCES})

target_include_directories(scsynth PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/synth/include/common
    ${CMAKE_CURRENT_SOURCE_DIR}/synth/include/plugin_interface
    ${CMAKE_CURRENT_SOURCE_DIR}/synth/include/server
    ${CMAKE_CURRENT_SOURCE_DIR}/synth/common
    ${CMAKE_CURRENT_SOURCE_DIR}/synth/server
    ${CMAKE_CURRENT_SOURCE_DIR}/synth/external_libraries
    ${CMAKE_CURRENT_SOURCE_DIR}/synth/external_libraries/nova-simd
    ${CMAKE_CURRENT_SOURCE_DIR}/synth/external_libraries/TLSF-2.4.6/src)

# TLSF: scsynth's real-time allocator. A C library, built separately because it
# must not inherit the C++ flags the engine uses.
add_library(scsynth_tlsf STATIC
    synth/external_libraries/TLSF-2.4.6/src/tlsf.c)
target_include_directories(scsynth_tlsf PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/synth/external_libraries/TLSF-2.4.6/src)
set_target_properties(scsynth_tlsf PROPERTIES POSITION_INDEPENDENT_CODE ON)

target_compile_definitions(scsynth PUBLIC
    # The engine's sources are upstream SuperCollider's with clockwork's changes
    # under this conditional (constexpr constants, the SAB node tree, the
    # guest's own commands, no dynamic plugin loading). PUBLIC because it shapes
    # headers, so anything that includes them must see the same value.
    CLOCKWORK_GUEST=1
    # No libsndfile, on every platform, as the web build has always been. The
    # engine has no files: /b_allocRead, /b_read, /b_readChannel,
    # /b_allocReadChannel and /b_write are answered by the CLIENT at the
    # gateway (SuperSonic's front natively, the client library on the web),
    # and the engine's own answer to them is /fail. Upstream's own
    # configuration; it shapes SC_HiddenWorld.h, so PUBLIC for the same
    # reason as above.
    NO_LIBSNDFILE=1)
# SC_AUDIO_API: scsynth's own headers refuse to compile without one, because
# upstream SuperCollider picked a device backend at compile time. It is
# vestigial here — clockwork owns the device and the engine never opens one —
# but the header still demands a value. 3 is what upstream SuperSonic passes.
target_compile_definitions(scsynth PRIVATE
    SC_AUDIO_API=3
    # Without this, PluginLoad(Chaos) expands to a dynamic plugin's anonymous
    # load() instead of Chaos_Load — the ugens compile, and the four whose
    # loaders have C++ linkage silently never register.
    STATIC_PLUGINS=1)

target_link_libraries(scsynth PRIVATE clockwork scsynth_tlsf)
set_target_properties(scsynth PROPERTIES POSITION_INDEPENDENT_CODE ON)
