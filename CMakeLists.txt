# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2025-2026 Sam Aaron
#
# SuperSonic = clockwork (the AGPL substrate) + scsynth (the GPL guest).
#
# There is deliberately very little here. Clockwork knows how to build itself
# and how to load a guest through src/dsp_api.h; this file's whole job is to
# say WHICH guest and WHERE, and then to build the host binary that is
# SuperSonic's own.
#
# Clockwork is a git submodule, maintained in its own repository.
cmake_minimum_required(VERSION 3.24)

project(SuperSonic
    VERSION 0.83.1
    DESCRIPTION "scsynth on the clockwork substrate"
    LANGUAGES C CXX)

# ─── The guest ───────────────────────────────────────────────────────────────
# dsp/scsynth defines a target `scsynth` implementing dsp_api.h. It lives here
# rather than inside clockwork for two reasons: clockwork must stay
# updatable without carrying an engine, and the engine is GPL-3.0-or-later
# while clockwork is AGPL-3.0-or-later or commercially licensed — two
# licences, and a boundary worth being able to see in the directory listing.
# What the binary calls itself. Clockwork is the substrate; the product is
# SuperSonic, and that is the name a user, a log file and another MIDI
# application should see.
#
# A DEFAULT, not a decree — hence no FORCE. An embedder ships this process
# under its own name because the OS identifies it by the executable: firewall
# consent prompts, Activity Monitor, ps and PipeWire's node name all show it,
# and a user of Sonic Pi should read "Sonic Pi" there rather than the name of a
# component they never installed. FORCE would have overwritten the embedder's
# -D silently, which is how a naming bug survives a build that looks correct.
if(NOT DEFINED CLOCKWORK_PRODUCT_NAME)
    set(CLOCKWORK_PRODUCT_NAME "SuperSonic" CACHE STRING "Name the binary reports to the OS")
endif()
# The banner, likewise a default: an embedder brings its own art or none.
if(NOT DEFINED CLOCKWORK_PRODUCT_HEADER)
    set(CLOCKWORK_PRODUCT_HEADER "${CMAKE_CURRENT_SOURCE_DIR}/dsp/supersonic_product.h"
        CACHE FILEPATH "Header defining the banner this product boots with")
endif()

# Ableton Link, on by default: SuperSonic is a drop-in scsynth for Sonic Pi,
# and Link tempo sync plus Link Audio are part of what it is. A DEFAULT, like
# the product name above — an embedder that wants a Link-free binary says so.
if(NOT DEFINED CLOCKWORK_LINK)
    set(CLOCKWORK_LINK ON CACHE BOOL "Integrate Ableton Link, tempo and audio (GPL-2.0-or-later; fetches the library)")
endif()
if(NOT DEFINED CLOCKWORK_LINK_AUDIO)
    set(CLOCKWORK_LINK_AUDIO ${CLOCKWORK_LINK} CACHE BOOL "Link peer audio in and publish out (needs CLOCKWORK_LINK)")
endif()

# The version the binary reports (banner, -v, the Windows version resource) is
# this project's, not clockwork's. Set before add_subdirectory: clockwork
# defaults CLOCKWORK_PRODUCT_VERSION to its own PROJECT_VERSION otherwise.
if(NOT DEFINED CLOCKWORK_PRODUCT_VERSION)
    set(CLOCKWORK_PRODUCT_VERSION "${PROJECT_VERSION}" CACHE STRING "Version the binary reports")
endif()

# Where an install puts the plugin bridge. Beside the server (bin) by default,
# which is how the engine finds it. A distribution that keeps helper programs
# out of PATH says libexec/supersonic instead; the engine is then told to look
# there too (CLOCKWORK_PLUGIN_BRIDGE_DIR), unless the embedder set that itself.
include(GNUInstallDirs)
set(SUPERSONIC_BRIDGE_INSTALL_DIR "${CMAKE_INSTALL_BINDIR}" CACHE STRING
    "Install directory for the plugin bridge, relative to the prefix or absolute")
if(NOT DEFINED CLOCKWORK_PLUGIN_BRIDGE_DIR
   AND NOT SUPERSONIC_BRIDGE_INSTALL_DIR STREQUAL CMAKE_INSTALL_BINDIR)
    if(IS_ABSOLUTE "${SUPERSONIC_BRIDGE_INSTALL_DIR}")
        set(_ss_bridge_dir "${SUPERSONIC_BRIDGE_INSTALL_DIR}")
    else()
        set(_ss_bridge_dir "${CMAKE_INSTALL_PREFIX}/${SUPERSONIC_BRIDGE_INSTALL_DIR}")
    endif()
    set(CLOCKWORK_PLUGIN_BRIDGE_DIR "${_ss_bridge_dir}" CACHE PATH
        "Directory the engine also searches for the plugin bridge (an installed layout)")
endif()

set(CLOCKWORK_DSP     "scsynth" CACHE STRING "" FORCE)
set(CLOCKWORK_DSP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/dsp/scsynth" CACHE PATH "" FORCE)

# ─── The Rust umbrella ───────────────────────────────────────────────────────
# Only one Rust staticlib may be linked into a binary, and SuperSonic has Rust
# of its own — the engine layer, where the OSC dispatch and the ugen host
# actually live. So clockwork's clockwork-native cannot be used: rust/
# supersonic-native depends on both halves and is built instead.
set(CLOCKWORK_RUST_WORKSPACE "${CMAKE_CURRENT_SOURCE_DIR}/rust" CACHE PATH   "" FORCE)
set(CLOCKWORK_RUST_PACKAGE   "supersonic-native"               CACHE STRING "" FORCE)
set(CLOCKWORK_RUST_LIB_NAME  "supersonic_native"               CACHE STRING "" FORCE)

# ─── Clockwork ───────────────────────────────────────────────────────────────
# CLOCKWORK_STANDALONE defaults off for a subdirectory build, so this brings
# in the library and the guest without clockwork's reference host or its own
# test suite.
add_subdirectory(clockwork)

# ─── SuperSonic ──────────────────────────────────────────────────────────────
# The process. SuperSonic's own main (host/), composed from the host pieces
# beside it and clockwork's comms client library: it opens the command
# socket, embeds the engine, stands its front (front/) between the two, and
# serves the segment. clockwork itself has no process shape.
add_executable(SuperSonic
    host/SuperSonicMain.cpp
    host/EngineHost.cpp
    front/SuperSonicFront.cpp
    front/Recorder.cpp)
add_dependencies(SuperSonic scsynth)
target_compile_definitions(SuperSonic PRIVATE
    SUPERSONIC_VERSION_STRING="${PROJECT_VERSION}")
target_include_directories(SuperSonic PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/host
    ${CMAKE_CURRENT_SOURCE_DIR}/front
    ${CMAKE_CURRENT_SOURCE_DIR}/clockwork/src/native   # the engine's device policy, event pump, mic guard
    ${CMAKE_CURRENT_SOURCE_DIR}/dsp/scsynth)   # buffer_commands.h: the guard frames
target_link_libraries(SuperSonic PRIVATE
    clockwork_comms
    clockwork_bundle)

# ─── Release debug symbols ───────────────────────────────────────────────────
# Clockwork sets these for everything it builds, but with add_compile_options /
# add_link_options — which apply to its own directory and below, not to a
# target declared up here. So the plugin bridge got a PDB and SuperSonic, the
# binary that actually ships, did not: a Release crash dump from it had nothing
# to symbolicate against. Restate them for this target rather than moving
# clockwork's, which are correct for clockwork.
if(MSVC)
    target_compile_options(SuperSonic PRIVATE "$<$<CONFIG:Release>:/Z7>")
    target_link_options(SuperSonic PRIVATE "$<$<CONFIG:Release>:/DEBUG;/OPT:REF;/OPT:ICF>")
else()
    target_compile_options(SuperSonic PRIVATE "$<$<CONFIG:Release>:-g>")
endif()

# ─── Hosted plugins ──────────────────────────────────────────────────────────
# Plugins run in clockwork's bridge process (clockwork/docs/TRACKS.md), which
# clockwork builds beside this binary as ${CLOCKWORK_PLUGIN_BRIDGE_NAME}; the
# engine finds it there by name. SuperSonic links no plugin code itself — the
# clockwork library carries TrackControl, the engine's side of the bridge, and
# publishes CLOCKWORK_HAS_PLUGIN_TRACKS to everything that links it. What
# is stated here is only that building SuperSonic builds the bridge too.
if(TARGET clockwork_plugin_bridge)
    add_dependencies(SuperSonic clockwork_plugin_bridge)
endif()

# ─── Tests ───────────────────────────────────────────────────────────────────
# SuperSonic's own suite, brought across from upstream: 99 files, 1085 cases.
# Clockwork's own tests stay in clockwork and run there — CLOCKWORK_STANDALONE
# keeps them out of this build.
if(BUILD_TESTS)
    enable_testing()
    add_subdirectory(test/native)
endif()

message(STATUS "SuperSonic ${PROJECT_VERSION}: clockwork + ${CLOCKWORK_DSP}")

# ─── Install ─────────────────────────────────────────────────────────────────
# What a package ships: the server as `supersonic` (the name Sonic Pi and the
# man page use), the plugin bridge, and the man page. The engine looks for the
# bridge beside its own executable first, then in CLOCKWORK_PLUGIN_BRIDGE_DIR;
# SUPERSONIC_BRIDGE_INSTALL_DIR (above) chooses which of those an install is.
include(GNUInstallDirs)
install(PROGRAMS $<TARGET_FILE:SuperSonic>
    DESTINATION ${CMAKE_INSTALL_BINDIR}
    RENAME supersonic)
if(TARGET clockwork_plugin_bridge)
    install(TARGETS clockwork_plugin_bridge
        RUNTIME DESTINATION ${SUPERSONIC_BRIDGE_INSTALL_DIR}
        BUNDLE  DESTINATION ${SUPERSONIC_BRIDGE_INSTALL_DIR})
endif()
install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/docs/man/supersonic.1
    DESTINATION ${CMAKE_INSTALL_MANDIR}/man1)
