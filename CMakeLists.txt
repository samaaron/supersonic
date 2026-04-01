cmake_minimum_required(VERSION 3.24)
project(SuperSonic VERSION 0.1.0 LANGUAGES C CXX)
if(APPLE)
    enable_language(OBJCXX)
endif()

# ─── C++ Standard ─────────────────────────────────────────────────────────────
# C++20 is required for std::atomic::wait() / notify_all()
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ─── Options ─────────────────────────────────────────────────────────────────
option(BUILD_TESTS "Build native Catch2 test suite" OFF)
option(BUILD_NIF   "Build BEAM NIF shared library"  OFF)

# NIF target requires all static libraries to be built with -fPIC
if(BUILD_NIF)
    set(CMAKE_POSITION_INDEPENDENT_CODE ON)
endif()

# ─── JUCE via FetchContent ────────────────────────────────────────────────────
include(FetchContent)

FetchContent_Declare(
    JUCE
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG        8.0.4
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(JUCE)

# Apply AUHAL patch to JUCE CoreAudio backend every configure (not just on fetch).
# This ensures the patch is always applied even when the JUCE source is cached.
if(APPLE)
    execute_process(
        COMMAND git checkout -- .
        WORKING_DIRECTORY "${juce_SOURCE_DIR}"
        ERROR_QUIET
    )
    execute_process(
        COMMAND git apply --whitespace=nowarn "${CMAKE_CURRENT_SOURCE_DIR}/patches/juce-auhal-coreaudio.patch"
        WORKING_DIRECTORY "${juce_SOURCE_DIR}"
        RESULT_VARIABLE _patch_result
    )
    if(NOT _patch_result EQUAL 0)
        message(WARNING "JUCE AUHAL patch failed to apply (result=${_patch_result})")
    endif()
endif()

# ─── libsndfile + audio codecs via FetchContent ──────────────────────────────
# Needed for /b_allocRead, /b_write, sample loading from disk.
# OVERRIDE_FIND_PACKAGE lets libsndfile's find_package() discover these
# FetchContent-built dependencies automatically (CMake 3.24+).

# Some codec libs have old cmake_minimum_required — CMake 4.x needs this
set(CMAKE_POLICY_VERSION_MINIMUM 3.5 CACHE STRING "" FORCE)

# Shared settings for all codec sub-projects
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(INSTALL_DOCS OFF CACHE BOOL "" FORCE)
set(INSTALL_MANPAGES OFF CACHE BOOL "" FORCE)

# Ogg (required by FLAC and Vorbis)
FetchContent_Declare(Ogg
    GIT_REPOSITORY https://github.com/xiph/ogg.git
    GIT_TAG        v1.3.5
    GIT_SHALLOW    TRUE
    OVERRIDE_FIND_PACKAGE
)

# Vorbis (for .ogg sample support)
FetchContent_Declare(Vorbis
    GIT_REPOSITORY https://github.com/xiph/vorbis.git
    GIT_TAG        v1.3.7
    GIT_SHALLOW    TRUE
    OVERRIDE_FIND_PACKAGE
)

# FLAC (for .flac sample support)
set(BUILD_CXXLIBS ON CACHE BOOL "" FORCE)
set(WITH_OGG ON CACHE BOOL "" FORCE)
FetchContent_Declare(FLAC
    GIT_REPOSITORY https://github.com/xiph/flac.git
    GIT_TAG        1.5.0
    GIT_SHALLOW    TRUE
    OVERRIDE_FIND_PACKAGE
)

# Opus (for .opus sample support)
FetchContent_Declare(Opus
    GIT_REPOSITORY https://github.com/xiph/opus.git
    GIT_TAG        v1.5.2
    GIT_SHALLOW    TRUE
    OVERRIDE_FIND_PACKAGE
)

# libsndfile — with FLAC, Vorbis, and Opus codec support
set(ENABLE_EXTERNAL_LIBS ON CACHE BOOL "" FORCE)
set(BUILD_REGTEST OFF CACHE BOOL "" FORCE)
set(ENABLE_MPEG OFF CACHE BOOL "" FORCE)
FetchContent_Declare(libsndfile
    GIT_REPOSITORY https://github.com/libsndfile/libsndfile.git
    GIT_TAG        1.2.2
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(Ogg Vorbis FLAC Opus)

# Vorbis's FetchContent build creates targets without namespace aliases.
# libsndfile expects Vorbis::vorbisenc and Vorbis::vorbisfile.
if(TARGET vorbisenc AND NOT TARGET Vorbis::vorbisenc)
    add_library(Vorbis::vorbisenc ALIAS vorbisenc)
endif()
if(TARGET vorbisfile AND NOT TARGET Vorbis::vorbisfile)
    add_library(Vorbis::vorbisfile ALIAS vorbisfile)
endif()

# Opus's FetchContent build exposes include/ as a flat directory (opus.h),
# but libsndfile expects #include <opus/opus.h> (installed layout).
# Create a wrapper include tree with the proper opus/ subdirectory.
set(_OPUS_WRAP_DIR "${CMAKE_BINARY_DIR}/_deps/_opus_include/opus")
file(MAKE_DIRECTORY "${_OPUS_WRAP_DIR}")
file(GLOB _OPUS_HDRS "${opus_SOURCE_DIR}/include/*.h")
foreach(_h ${_OPUS_HDRS})
    get_filename_component(_name "${_h}" NAME)
    configure_file("${_h}" "${_OPUS_WRAP_DIR}/${_name}" COPYONLY)
endforeach()
if(TARGET opus)
    target_include_directories(opus PUBLIC $<BUILD_INTERFACE:${CMAKE_BINARY_DIR}/_deps/_opus_include>)
endif()
if(TARGET opus AND NOT TARGET Opus::opus)
    add_library(Opus::opus ALIAS opus)
endif()

FetchContent_MakeAvailable(libsndfile)

# ─── Paths ────────────────────────────────────────────────────────────────────
set(SUPERSONIC_SRC  "${CMAKE_CURRENT_SOURCE_DIR}/src")
set(NATIVE_SRC      "${CMAKE_CURRENT_SOURCE_DIR}/src/native")
set(WORKERS_SRC     "${CMAKE_CURRENT_SOURCE_DIR}/src/workers")

# ─── Source files ─────────────────────────────────────────────────────────────

# Engine sources (everything except Main.cpp — shared between exe and tests)
set(ENGINE_SOURCES
    ${NATIVE_SRC}/HeadlessDriver.cpp
    ${NATIVE_SRC}/JuceAudioCallback.cpp
    ${NATIVE_SRC}/OscUdpServer.cpp
    ${NATIVE_SRC}/SampleLoader.cpp
    ${NATIVE_SRC}/SupersonicEngine.cpp
    ${NATIVE_SRC}/AggregateDeviceHelper.cpp
    ${NATIVE_SRC}/StateCache.cpp
    ${WORKERS_SRC}/Prescheduler.cpp
    ${WORKERS_SRC}/ReplyReader.cpp
    ${WORKERS_SRC}/DebugReader.cpp
)

# SuperSonic core (audio_processor + companions)
set(SUPERSONIC_CORE_SOURCES
    ${SUPERSONIC_SRC}/audio_processor.cpp
    ${SUPERSONIC_SRC}/buffer_commands.cpp
    ${SUPERSONIC_SRC}/node_tree.cpp
    ${SUPERSONIC_SRC}/uuid_rewriter.cpp
    ${SUPERSONIC_SRC}/supersonic_heap.cpp
)

# scsynth server (excludes WASM-only and platform-specific driver files)
set(SCSYNTH_SERVER_SOURCES
    ${SUPERSONIC_SRC}/scsynth/server/SC_BufGen.cpp
    ${SUPERSONIC_SRC}/scsynth/server/SC_Graph.cpp
    ${SUPERSONIC_SRC}/scsynth/server/SC_GraphDef.cpp
    ${SUPERSONIC_SRC}/scsynth/server/SC_Group.cpp
    ${SUPERSONIC_SRC}/scsynth/server/SC_Lib.cpp
    ${SUPERSONIC_SRC}/scsynth/server/SC_Lib_Cintf.cpp
    ${SUPERSONIC_SRC}/scsynth/server/SC_MiscCmds.cpp
    ${SUPERSONIC_SRC}/scsynth/server/SC_Node.cpp
    ${SUPERSONIC_SRC}/scsynth/server/SC_OscUnroll.cpp
    ${SUPERSONIC_SRC}/scsynth/server/SC_Rate.cpp
    ${SUPERSONIC_SRC}/scsynth/server/SC_SequencedCommand.cpp
    ${SUPERSONIC_SRC}/scsynth/server/SC_Str4.cpp
    ${SUPERSONIC_SRC}/scsynth/server/SC_Stubs.cpp
    ${SUPERSONIC_SRC}/scsynth/server/SC_Unit.cpp
    ${SUPERSONIC_SRC}/scsynth/server/SC_UnitDef.cpp
    ${SUPERSONIC_SRC}/scsynth/server/SC_World.cpp
    # SC_WebAudio.cpp, SC_Wasm.cpp, SC_WasmOscBuilder.cpp — excluded (WASM-only)
)

# scsynth common (cross-platform subset)
set(SCSYNTH_COMMON_SOURCES
    ${SUPERSONIC_SRC}/scsynth/common/SC_AllocPool.cpp
    ${SUPERSONIC_SRC}/scsynth/common/SC_Errors.cpp
    ${SUPERSONIC_SRC}/scsynth/common/SC_Reply.cpp
    ${SUPERSONIC_SRC}/scsynth/common/SC_ServerBootDelayWarning.cpp
    ${SUPERSONIC_SRC}/scsynth/common/SC_StringBuffer.cpp
    ${SUPERSONIC_SRC}/scsynth/common/SC_StringParser.cpp
    ${SUPERSONIC_SRC}/scsynth/common/SC_TextUtils.cpp
    ${SUPERSONIC_SRC}/scsynth/common/SC_fftlib.cpp
    ${SUPERSONIC_SRC}/scsynth/common/Samp.cpp
    ${SUPERSONIC_SRC}/scsynth/common/fftlib.c
)

if(WIN32)
    list(APPEND SCSYNTH_COMMON_SOURCES
        ${SUPERSONIC_SRC}/scsynth/common/SC_Filesystem_win.cpp
        ${SUPERSONIC_SRC}/scsynth/common/SC_Win32Utils.cpp
    )
elseif(APPLE)
    # SC_Filesystem_macos.cpp includes Foundation headers — must compile as ObjC++
    set_source_files_properties(
        ${SUPERSONIC_SRC}/scsynth/common/SC_Filesystem_macos.cpp
        PROPERTIES LANGUAGE OBJCXX
    )
    list(APPEND SCSYNTH_COMMON_SOURCES
        ${SUPERSONIC_SRC}/scsynth/common/SC_Filesystem_macos.cpp
        ${SUPERSONIC_SRC}/scsynth/common/SC_Apple.mm
    )
elseif(UNIX)
    list(APPEND SCSYNTH_COMMON_SOURCES
        ${SUPERSONIC_SRC}/scsynth/common/SC_Filesystem_unix.cpp
    )
endif()

# scsynth plugins
set(SCSYNTH_PLUGIN_SOURCES
    ${SUPERSONIC_SRC}/scsynth/plugins/BinaryOpUGens.cpp
    ${SUPERSONIC_SRC}/scsynth/plugins/ChaosUGens.cpp
    ${SUPERSONIC_SRC}/scsynth/plugins/Convolution.cpp
    ${SUPERSONIC_SRC}/scsynth/plugins/DelayUGens.cpp
    ${SUPERSONIC_SRC}/scsynth/plugins/DemandUGens.cpp
    ${SUPERSONIC_SRC}/scsynth/plugins/DemoUGens.cpp
    ${SUPERSONIC_SRC}/scsynth/plugins/DynNoiseUGens.cpp
    ${SUPERSONIC_SRC}/scsynth/plugins/FFT2InterfaceTable.cpp
    ${SUPERSONIC_SRC}/scsynth/plugins/FFTInterfaceTable.cpp
    ${SUPERSONIC_SRC}/scsynth/plugins/FFT_UGens.cpp
    ${SUPERSONIC_SRC}/scsynth/plugins/FeatureDetection.cpp
    ${SUPERSONIC_SRC}/scsynth/plugins/FilterUGens.cpp
    ${SUPERSONIC_SRC}/scsynth/plugins/GendynUGens.cpp
    ${SUPERSONIC_SRC}/scsynth/plugins/GrainUGens.cpp
    ${SUPERSONIC_SRC}/scsynth/plugins/IOUGens.cpp
    ${SUPERSONIC_SRC}/scsynth/plugins/LFUGens.cpp
    ${SUPERSONIC_SRC}/scsynth/plugins/MulAddUGens.cpp
    ${SUPERSONIC_SRC}/scsynth/plugins/NoiseUGens.cpp
    ${SUPERSONIC_SRC}/scsynth/plugins/OscUGens.cpp
    ${SUPERSONIC_SRC}/scsynth/plugins/PV_ThirdParty.cpp
    ${SUPERSONIC_SRC}/scsynth/plugins/PV_UGens.cpp
    ${SUPERSONIC_SRC}/scsynth/plugins/PanUGens.cpp
    ${SUPERSONIC_SRC}/scsynth/plugins/PartitionedConvolution.cpp
    ${SUPERSONIC_SRC}/scsynth/plugins/PhysicalModelingUGens.cpp
    ${SUPERSONIC_SRC}/scsynth/plugins/ReverbUGens.cpp
    ${SUPERSONIC_SRC}/scsynth/plugins/TestUGens.cpp
    ${SUPERSONIC_SRC}/scsynth/plugins/TriggerUGens.cpp
    ${SUPERSONIC_SRC}/scsynth/plugins/UnaryOpUGens.cpp
    ${SUPERSONIC_SRC}/scsynth/plugins/UnpackFFTUGens.cpp
    ${SUPERSONIC_SRC}/scsynth/plugins/sc3-plugins/DistortionUGens.cpp
    # MdaUGens.cpp excluded from build (see upstream commit faff87a)
)

# oscpack vendor (same sources as build.sh)
set(OSCPACK_SOURCES
    ${SUPERSONIC_SRC}/vendor/oscpack/osc/OscTypes.cpp
    ${SUPERSONIC_SRC}/vendor/oscpack/osc/OscOutboundPacketStream.cpp
    ${SUPERSONIC_SRC}/vendor/oscpack/osc/OscReceivedElements.cpp
)

# ─── Common include directories ─────────────────────────────────────────────
# Collected into a variable so both the engine lib and exe can share them.
set(SUPERSONIC_INCLUDE_DIRS
    # NativeShim first — overrides emscripten/ includes
    ${NATIVE_SRC}/NativeShim

    # SuperSonic native + workers src
    ${NATIVE_SRC}
    ${WORKERS_SRC}

    # SuperSonic source root (for #include "shared_memory.h" etc.)
    ${SUPERSONIC_SRC}

    # Project root (for #include "supersonic/src/..." relative paths)
    ${CMAKE_CURRENT_SOURCE_DIR}

    # scsynth include paths (mirror build.sh)
    ${SUPERSONIC_SRC}/scsynth/include/common
    ${SUPERSONIC_SRC}/scsynth/include/plugin_interface
    ${SUPERSONIC_SRC}/scsynth/include/server
    ${SUPERSONIC_SRC}/scsynth/server
    ${SUPERSONIC_SRC}/scsynth/common
    ${SUPERSONIC_SRC}/scsynth/external_libraries/boost
    ${SUPERSONIC_SRC}/scsynth/external_libraries/nova-simd

    # oscpack vendor
    ${SUPERSONIC_SRC}/vendor/oscpack
)

# ─── Common compile definitions ──────────────────────────────────────────────
set(SUPERSONIC_COMPILE_DEFS
    SUPERSONIC=1
    STATIC_PLUGINS=1
    NOVA_SIMD=1
    SC_FFT_GREEN=1
    NDEBUG=1
    NOMINMAX=1
    $<$<PLATFORM_ID:Windows>:WIN32_LEAN_AND_MEAN=1>

    SCHEDULER_SLOT_SIZE=1024
    SCHEDULER_SLOT_COUNT=512
    NODE_TREE_MIRROR_MAX_NODES=1024

    # Native builds need an SC_AUDIO_API defined — we use JUCE, not any SC backend,
    # but the header requires a value.  PORTAUDIO (3) is neutral and includes no
    # platform-specific headers.
    SC_AUDIO_API=3
)

# ─── TLSF memory allocator (for scope buffer pool in shared memory) ─────────
add_library(tlsf STATIC
    ${SUPERSONIC_SRC}/scsynth/external_libraries/TLSF-2.4.6/src/tlsf.c
)
target_include_directories(tlsf PUBLIC
    ${SUPERSONIC_SRC}/scsynth/external_libraries/TLSF-2.4.6/src
)
target_compile_definitions(tlsf PRIVATE TLSF_STATISTIC=0)
set_property(TARGET tlsf PROPERTY POSITION_INDEPENDENT_CODE TRUE)
if(MSVC)
    target_compile_options(tlsf PRIVATE /W0)
endif()

# ─── supersonic_engine static library ────────────────────────────────────────
# Shared between the executable and the test suite.
add_library(supersonic_engine STATIC
    ${ENGINE_SOURCES}
    ${SUPERSONIC_CORE_SOURCES}
    ${SCSYNTH_SERVER_SOURCES}
    ${SCSYNTH_COMMON_SOURCES}
    ${SCSYNTH_PLUGIN_SOURCES}
    ${OSCPACK_SOURCES}
)

target_include_directories(supersonic_engine PUBLIC ${SUPERSONIC_INCLUDE_DIRS})
target_compile_definitions(supersonic_engine PUBLIC ${SUPERSONIC_COMPILE_DEFS})

# Disable JUCE modules we don't need (avoids curl dependency on Linux)
target_compile_definitions(supersonic_engine PUBLIC
    JUCE_USE_CURL=0
    JUCE_WEB_BROWSER=0
)

target_link_libraries(supersonic_engine PUBLIC
    juce::juce_core
    juce::juce_audio_basics
    juce::juce_audio_devices
    juce::juce_audio_formats
    SndFile::sndfile
    tlsf
)

if(WIN32)
    target_link_libraries(supersonic_engine PUBLIC Ws2_32 winmm)
elseif(APPLE)
    target_link_libraries(supersonic_engine PUBLIC
        "-framework CoreAudio" "-framework AudioUnit"
        "-framework Foundation" "-framework CoreFoundation")
elseif(UNIX)
    find_package(Threads REQUIRED)
    target_link_libraries(supersonic_engine PUBLIC Threads::Threads rt)
endif()

if(MSVC)
    target_compile_options(supersonic_engine PRIVATE
        /W1 /wd4244 /wd4267 /wd4305 /wd4800 /wd4996 /permissive-
    )
else()
    target_compile_options(supersonic_engine PRIVATE
        -Wno-deprecated-declarations -Wno-unused-parameter
        -fno-math-errno -fsigned-zeros
    )
endif()

# ─── SuperSonic executable (thin wrapper) ────────────────────────────────────
juce_add_console_app(SuperSonic
    PRODUCT_NAME    "supersonic"
    VERSION         "0.1.0"
    COMPANY_NAME    "SuperSonic"
)

target_sources(SuperSonic PRIVATE ${NATIVE_SRC}/Main.cpp)
target_link_libraries(SuperSonic PRIVATE supersonic_engine)

target_compile_definitions(SuperSonic PRIVATE
    JUCE_APPLICATION_NAME_STRING="$<TARGET_PROPERTY:SuperSonic,JUCE_PRODUCT_NAME>"
    JUCE_APPLICATION_VERSION_STRING="$<TARGET_PROPERTY:SuperSonic,JUCE_VERSION>"
)

# ─── BEAM NIF shared library ─────────────────────────────────────────────────
# Bundled Erlang NIF headers from OTP 29-rc1 (Apache-2.0).
# erl_int_sizes_config.h is generated at build time via CMake check_type_size().
if(BUILD_NIF)
    set(ERL_HEADERS_DIR ${CMAKE_CURRENT_SOURCE_DIR}/external_libs/erlang_headers)

    # Generate erl_int_sizes_config.h for the target platform
    include(CheckTypeSize)
    check_type_size("char"      SIZEOF_CHAR      BUILTIN_TYPES_ONLY LANGUAGE C)
    check_type_size("short"     SIZEOF_SHORT     BUILTIN_TYPES_ONLY LANGUAGE C)
    check_type_size("int"       SIZEOF_INT       BUILTIN_TYPES_ONLY LANGUAGE C)
    check_type_size("long"      SIZEOF_LONG      BUILTIN_TYPES_ONLY LANGUAGE C)
    check_type_size("long long" SIZEOF_LONG_LONG BUILTIN_TYPES_ONLY LANGUAGE C)
    # CMAKE_SIZEOF_VOID_P is set automatically by CMake
    configure_file(
        ${ERL_HEADERS_DIR}/erl_int_sizes_config.h.in
        ${CMAKE_CURRENT_BINARY_DIR}/erlang_generated/erl_int_sizes_config.h
    )

    add_library(supersonic_nif SHARED src/nif/supersonic_nif.cpp)
    set_target_properties(supersonic_nif PROPERTIES
        PREFIX ""
        OUTPUT_NAME "supersonic"
        # Put the NIF in the build root on all platforms (not Release/ or Debug/)
        # so erlang:load_nif finds it at a consistent path.
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
        LIBRARY_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}"
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}"
        LIBRARY_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}"
        RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}"
    )
    target_include_directories(supersonic_nif PRIVATE
        ${CMAKE_CURRENT_BINARY_DIR}/erlang_generated   # generated erl_int_sizes_config.h
        ${ERL_HEADERS_DIR}                              # bundled OTP 29 headers
    )
    target_link_libraries(supersonic_nif PRIVATE supersonic_engine)

    if(APPLE)
        # erlang:load_nif expects .so on all Unix platforms (not .dylib)
        set_target_properties(supersonic_nif PROPERTIES
            SUFFIX ".so"
            LINK_FLAGS "-undefined dynamic_lookup")
    endif()
    if(UNIX AND NOT APPLE)
        # Linux already defaults to .so, but be explicit
        set_target_properties(supersonic_nif PROPERTIES SUFFIX ".so")
    endif()
endif()

# ─── Tests ───────────────────────────────────────────────────────────────────
if(BUILD_TESTS)
    add_subdirectory(test/native)
endif()
