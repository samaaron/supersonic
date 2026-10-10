# This directory is SuperSonic's test suite wherever SuperSonic sits: built at
# the top level or as somebody's subproject (Sonic Pi's, the embed guard's).
# So every path here anchors on this file, never on CMAKE_SOURCE_DIR, which is
# the EMBEDDER's root in the second case.
get_filename_component(SUPERSONIC_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)

# Prefer a system Catch2 v3 (distro package-builds run offline and Debian ships
# one); fall back to the pinned FetchContent copy for dev machines without it.
find_package(Catch2 3 QUIET)
if(NOT TARGET Catch2::Catch2)
    include(FetchContent)

    FetchContent_Declare(
        Catch2
        GIT_REPOSITORY https://github.com/catchorg/Catch2.git
        GIT_TAG        v3.5.2
        GIT_SHALLOW    TRUE
        ${_ss_fc_exclude}  # keep Catch2's install() rules out of our manifest
    )
    FetchContent_MakeAvailable(Catch2)
    list(APPEND CMAKE_MODULE_PATH "${Catch2_SOURCE_DIR}/extras")
else()
    message(STATUS "Catch2: system install ${Catch2_VERSION}")
    # Catch2's package config normally appends its own dir to CMAKE_MODULE_PATH
    # for the include(Catch) below; add it explicitly in case a distro's
    # packaging of the config omits that.
    list(APPEND CMAKE_MODULE_PATH "${Catch2_DIR}")
endif()

# NOT test_event_scheduler.cpp or test_midi_clock_out.cpp. Both test code that
# now lives in clockwork, and both are covered there more thoroughly than
# they were here:
#
#   the scheduler   all 9 of the cases that were here have an equivalent in
#                   clockwork/test/test_scheduler.cpp — three of them added
#                   BECAUSE this comparison found them missing, and two of
#                   those three found real bugs in the Rust store.
#   MIDI clock out  9 cases against the 2 that were here, including delivery
#                   through a real port. The ones here asserted ticks land in
#                   the SCHEDULER, which stopped being true when MIDI clock
#                   moved onto event sinks: it is TauClock's, not the queue's.
#
# A test belongs with the code it tests. These followed theirs.
add_executable(SuperSonicNativeTests
    test_main.cpp
    test_sc_calc.cpp
    test_boot.cpp
    test_osc_commands.cpp
    test_synthdef.cpp
    test_embedded_pools.cpp
    test_synth_lifecycle.cpp
    test_group_commands.cpp
    test_node_tree.cpp
    test_completion_message.cpp
    test_osc_semantic.cpp
    test_stress.cpp
    test_buffer_advanced.cpp
    test_control_bus.cpp
    test_synth_commands.cpp
    test_synthdef_versions.cpp
    test_error_handling.cpp
    test_lifecycle_leaks.cpp
    test_callbacks.cpp
    test_metrics.cpp
    test_node_tree_identity.cpp
    test_node_tree_mirror.cpp
    test_load_sample.cpp
    test_sample_load_offthread.cpp
    test_load_sample_origin.cpp
    test_piano_wavetable.cpp
    test_supersonic_front.cpp
    test_engine_host.cpp
    test_supersonic_binary.cpp
    test_front_recording.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../../front/supersonic_commands.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../../front/Recorder.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../../host/EngineHost.cpp
    test_device_management.cpp
    test_definition_lifecycle.cpp
    test_send_reply.cpp
    test_randid.cpp
    test_control_endpoints.cpp
    test_multichannel.cpp
    test_sound_out_fx.cpp
    test_scheduling_accuracy.cpp
    test_scsynth_heap.cpp
    test_cold_swap.cpp
    test_rebuild_contract.cpp
    test_cold_swap_scope.cpp
    test_timing_accuracy.cpp
    test_cold_swap_buffers.cpp
    test_cold_swap_send_reply.cpp
    test_notify_clients.cpp
    test_stream_client_registration.cpp
    test_input_channels.cpp
    test_rapid_switching.cpp
    test_splimiter.cpp
    test_recover_audio.cpp
    test_scope_race.cpp
    test_heap_growth.cpp
    test_benchmark.cpp
    test_bundle_sequential.cpp
    test_clear_sched.cpp
    test_rt_alloc_guest.cpp
    test_graphdef_leak.cpp
    test_scope_slot_ownership.cpp
    test_buffer_alloc_bounds.cpp
    OscTestUtils.cpp
    EngineFixture.cpp
    FakeLinkPeerProcess.cpp
    test_link_audio_integration.cpp
    test_link_ugen.cpp
)

target_link_libraries(SuperSonicNativeTests PRIVATE
    # The embedder's way in: one target carrying clockwork, the guest and the
    # Rust umbrella, with the link group the clockwork/DSP cycle needs.
    clockwork_bundle
    clockwork_comms        # the command transports: a client library, not the engine's
    Catch2::Catch2
)

target_include_directories(SuperSonicNativeTests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../../front
    ${CMAKE_CURRENT_SOURCE_DIR}/../../host
    ${CMAKE_CURRENT_SOURCE_DIR}/../../clockwork/src/native
    # SPLimiterCore.h is a plain header with no SC dependencies, so
    # test_splimiter.cpp includes it directly rather than going through
    # the UGen.
    # The engine's headers. The tests reach into scsynth directly — that is
    # what most of them are for — so they need what the guest needs. These do
    # not arrive through clockwork_bundle because the link group carries
    # FILES, which have no usage requirements to propagate.
    ${SUPERSONIC_ROOT}/dsp/scsynth
    ${SUPERSONIC_ROOT}/dsp/scsynth/synth/plugins
    ${SUPERSONIC_ROOT}/dsp/scsynth/synth/common
    ${SUPERSONIC_ROOT}/dsp/scsynth/synth/server
    ${SUPERSONIC_ROOT}/dsp/scsynth/synth/include/common
    ${SUPERSONIC_ROOT}/dsp/scsynth/synth/include/plugin_interface
    ${SUPERSONIC_ROOT}/dsp/scsynth/synth/include/server
)
# …and for the same reason, the guest's PUBLIC definition does not arrive
# either. Its headers change shape under it.
target_compile_definitions(SuperSonicNativeTests PRIVATE CLOCKWORK_GUEST=1)

target_compile_definitions(SuperSonicNativeTests PRIVATE
    CLOCKWORK_SYNTHDEFS_DIR="${SUPERSONIC_ROOT}/packages/supersonic-scsynth-synthdefs/synthdefs"
    # The process itself, for test_supersonic_binary.cpp to spawn.
    SUPERSONIC_BINARY="$<TARGET_FILE:SuperSonic>"
    CLOCKWORK_TEST_SYNTHDEFS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../synthdefs"
    CLOCKWORK_SAMPLES_DIR="${SUPERSONIC_ROOT}/packages/supersonic-scsynth-samples/samples"
    JUCE_APPLICATION_NAME_STRING="supersonic-tests"
    JUCE_APPLICATION_VERSION_STRING="${PROJECT_VERSION}"
)

if(MSVC)
    target_compile_options(SuperSonicNativeTests PRIVATE /W1 /wd4244 /wd4267 /wd4305 /permissive-)
    target_link_options(SuperSonicNativeTests PRIVATE /FORCE:MULTIPLE)
endif()

# ─── Out-of-process Link peer used by integration tests ────────────────────
# Minimal Link Audio publisher with CLI-configurable channels and
# deterministic audio generators. Tests spawn it via FakeLinkPeerProcess
# and assert on what the engine receives. Only built when Link is on.
#
# The guard was CLOCKWORK_ENABLE_LINK, which is a name nothing in this tree sets —
# it appeared here and nowhere else. So this target was never built and
# CLOCKWORK_TEST_LINK_PEER_BINARY was never defined, which FakeLinkPeerProcess.cpp
# needs to compile at all. The option is CLOCKWORK_LINK.
if(CLOCKWORK_LINK)
    # Publishes from a real-time thread, as the apps it stands in for do from
    # their audio callbacks: clockwork's own helper, compiled in.
    add_executable(supersonic_test_link_peer
        peer/main.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../../clockwork/src/native/RealtimeThread.cpp
    )
    target_include_directories(supersonic_test_link_peer PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../../clockwork/src/native
    )
    target_link_libraries(supersonic_test_link_peer PRIVATE
        Ableton::Link
    )
    # Link's headers are C++17 (std::optional, std::invoke_result_t) and the
    # Ableton::Link interface target does not state a standard, so this target
    # would otherwise take the compiler's default and fail inside Link itself.
    # Matches the rest of the project rather than Link's minimum.
    set_target_properties(supersonic_test_link_peer PROPERTIES
        CXX_STANDARD 20
        CXX_STANDARD_REQUIRED ON
    )
    target_compile_definitions(SuperSonicNativeTests PRIVATE
        CLOCKWORK_TEST_LINK_PEER_BINARY="$<TARGET_FILE:supersonic_test_link_peer>"
    )
    add_dependencies(SuperSonicNativeTests supersonic_test_link_peer)
endif()

include(CTest)
include(Catch)
# Discovery reads the test list as UTF-8 (cmake/CatchAddTestsUtf8.cmake.in
# says why): Catch2's own script, included under the policy that makes CMake
# decode a process's output that way on Windows.
set(CATCH_ADD_TESTS_SCRIPT "${_CATCH_DISCOVER_TESTS_SCRIPT}")
configure_file(cmake/CatchAddTestsUtf8.cmake.in "${CMAKE_CURRENT_BINARY_DIR}/CatchAddTestsUtf8.cmake" @ONLY)
set(_CATCH_DISCOVER_TESTS_SCRIPT "${CMAKE_CURRENT_BINARY_DIR}/CatchAddTestsUtf8.cmake"
    CACHE INTERNAL "Catch2's CatchAddTests.cmake, run with its output read as UTF-8" FORCE)
# One CTest entry per Catch2 case, which is how CI runs the suite (ctest, not
# the binary): everything registered here runs, including the guest boundary
# check below. Benchmarks are left out of the discovery, as CI has always
# left them out of the run; run the binary with "[benchmark]" for them.
#
# A CASE'S NAME MUST NOT OPEN A SQUARE BRACKET IT DOES NOT CLOSE. The
# discovered names travel through a CMake list, where an unbalanced `[`
# swallows every `;` up to the next `]` — so "phase in [0, quantum)" fused
# itself and the six cases after it into one entry that matched nothing and
# failed. Tags are fine; prose in the name is not.
catch_discover_tests(SuperSonicNativeTests TEST_SPEC "~[benchmark]~[Link]")
# The Link cases one at a time: Link peers on one machine share a session, so
# two at once see each other's peers and set each other's tempo.
catch_discover_tests(SuperSonicNativeTests TEST_SPEC "[Link]~[benchmark]"
                     PROPERTIES RESOURCE_LOCK link_session)

# test_supersonic_binary.cpp spawns the SuperSonic process: build it first.
add_dependencies(SuperSonicNativeTests SuperSonic)

# ─── The guest boundary ──────────────────────────────────────────────────────
# The scsynth guest reaches clockwork through dsp_api.h and nothing else:
# scripts/check-guest-boundary.sh greps the guest's sources for any include
# or symbol of clockwork's past that header, and fails on the first one.
find_program(SUPERSONIC_SH sh)
if(SUPERSONIC_SH)
    add_test(NAME guest_boundary
        COMMAND "${SUPERSONIC_SH}" "${SUPERSONIC_ROOT}/scripts/check-guest-boundary.sh")
else()
    message(STATUS "no sh on this machine: the guest boundary check is not registered here (CI runs it on Linux)")
endif()
