# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (c) 2026 Sam Aaron
#
# Fuzz targets for the scsynth code that reads untrusted bytes: the SynthDef
# reader and the command handlers. Each links the whole engine (Clockwork and
# the scsynth guest) and drives it from the calling thread (engine_harness.h).
# Built only with -DSUPERSONIC_FUZZ=ON, in a build directory of its own,
# because the whole tree has to be compiled with the fuzzing instrumentation:
#
#   CC=$(brew --prefix llvm)/bin/clang CXX=$(brew --prefix llvm)/bin/clang++ \
#   cmake -S . -B build/fuzz -DSUPERSONIC_FUZZ=ON \
#     -DCLOCKWORK_LINK=OFF -DCLOCKWORK_PLUGINS=OFF -DCLOCKWORK_NIF=OFF \
#     -DCMAKE_BUILD_TYPE=RelWithDebInfo \
#     "-DCMAKE_C_FLAGS=-fsanitize=fuzzer-no-link,address,undefined -fno-sanitize=function -fno-sanitize-recover=undefined" \
#     "-DCMAKE_CXX_FLAGS=-fsanitize=fuzzer-no-link,address,undefined -fno-sanitize=function -fno-sanitize-recover=undefined"
#   cmake --build build/fuzz --target fuzz_synthdef fuzz_scsynth_commands
#   build/fuzz/test/fuzz/fuzz_synthdef -max_total_time=300 build/fuzz/corpus-synthdef \
#     packages/supersonic-scsynth-synthdefs/synthdefs test/synthdefs test/fuzz/corpus/synthdef
#
# -fno-sanitize=function: scsynth calls every node and unit generator through a
# pointer to its base type's calc function (Group_Calc through a Node pointer,
# SinOsc_next through a Unit pointer), as upstream SuperCollider does. The
# check reports each call; it is not a memory error, and the rest of UBSan
# stays on.
#
# With a compiler that has no libFuzzer, each target replays the files it is
# given through Clockwork's replay main (clockwork/test/fuzz/replay_main.cpp).

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    set(SUPERSONIC_FUZZ_LINK -fsanitize=fuzzer)
    set(SUPERSONIC_FUZZ_REPLAY "")
else()
    set(SUPERSONIC_FUZZ_LINK "")
    set(SUPERSONIC_FUZZ_REPLAY ${CMAKE_SOURCE_DIR}/clockwork/test/fuzz/replay_main.cpp)
endif()

function(supersonic_fuzz_target name)
    add_executable(${name} ${ARGN} ${SUPERSONIC_FUZZ_REPLAY})
    add_dependencies(${name} scsynth)
    target_include_directories(${name} PRIVATE
        ${CMAKE_SOURCE_DIR}/clockwork/src
        ${CMAKE_SOURCE_DIR}/clockwork/src/native)
    target_compile_definitions(${name} PRIVATE
        SUPERSONIC_SYNTHDEFS_DIR="${CMAKE_SOURCE_DIR}/packages/supersonic-scsynth-synthdefs/synthdefs")
    target_link_libraries(${name} PRIVATE clockwork_bundle)
    target_link_options(${name} PRIVATE ${SUPERSONIC_FUZZ_LINK})
endfunction()

supersonic_fuzz_target(fuzz_synthdef fuzz_synthdef.cpp)
supersonic_fuzz_target(fuzz_scsynth_commands fuzz_scsynth_commands.cpp)
