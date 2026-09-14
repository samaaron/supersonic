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
    test_platform.cpp
    test_sc_calc.cpp
    test_boot.cpp
    test_boot_input_pairing.cpp
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
    test_config_validation.cpp
    test_engine_lifecycle.cpp
    test_lifecycle_leaks.cpp
    test_callbacks.cpp
    test_metrics.cpp
    test_metrics_shm.cpp
    test_node_tree_identity.cpp
    test_node_tree_mirror.cpp
    test_osc_codec.cpp
    test_load_sample.cpp
    test_sample_load_offthread.cpp
    test_load_sample_origin.cpp
    test_piano_wavetable.cpp
    test_supersonic_front.cpp
    test_engine_host.cpp
    test_supersonic_binary.cpp
    test_front_recording.cpp
    test_master_tap.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../../front/SuperSonicFront.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../../front/Recorder.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../../host/EngineHost.cpp
    test_device_management.cpp
    test_definition_lifecycle.cpp
    test_send_reply.cpp
    test_control_endpoints.cpp
    test_osc_ingress.cpp
    test_multichannel.cpp
    test_sound_out_fx.cpp
    test_audio_resume.cpp
    test_fuzzy_match.cpp
    test_scheduling_accuracy.cpp
    test_engine_state.cpp
    test_cold_swap.cpp
    test_rebuild_contract.cpp
    test_cold_swap_recovery.cpp
    test_cold_swap_scope.cpp
    test_timing_accuracy.cpp
    test_cold_swap_buffers.cpp
    test_cold_swap_send_reply.cpp
    test_notify_clients.cpp
    test_stream_client_registration.cpp
    test_guest_config_layout.cpp
    test_input_channels.cpp
    test_driver_switch.cpp
    test_rapid_switching.cpp
    test_callback_watchdog.cpp
    test_hardening_policy.cpp
    test_control_plane_liveness.cpp
    test_time_source.cpp
    test_device_selection.cpp
    test_aggregate_buffer_clamp.cpp
    test_device_policy.cpp
    test_splimiter.cpp
    test_audio_recovery.cpp
    test_recover_audio.cpp
    test_scope_race.cpp
    test_heap_growth.cpp
    test_scheduler.cpp
    test_ring_buffer_write.cpp
    test_ring_reader.cpp
    test_ring_concurrency.cpp
    test_benchmark.cpp
    test_bundle_sequential.cpp
    test_input_fallback.cpp
    test_no_audio_device.cpp
    test_swap_characterization.cpp
    test_rt_alloc.cpp
    test_graphdef_leak.cpp
    test_reply_routing.cpp
    test_in_ring_drain.cpp
    test_scope_slot_ownership.cpp
    test_scope_stream.cpp
    test_server_shm_security.cpp
    test_buffer_alloc_bounds.cpp
    test_realtime_thread.cpp
    test_lanes.cpp
    test_ring_wire_conformance.cpp
    OscTestUtils.cpp
    EngineFixture.cpp
    FakeLinkPeerProcess.cpp
    test_link_audio_integration.cpp
    test_link_ugen.cpp
)

# test_midi exercises the gated midir backend (MidiControl/ss_midi); the other MIDI
# tests cover the always-compiled core (EngineScheduler/MidiClockOut).
if(CLOCKWORK_ENABLE_MIDI)
    target_sources(SuperSonicNativeTests PRIVATE test_midi.cpp)
endif()

# test_gamepad exercises the gated gilrs backend (GamepadControl/ss_gamepad).
if(CLOCKWORK_ENABLE_GAMEPAD)
    target_sources(SuperSonicNativeTests PRIVATE test_gamepad.cpp)
endif()

# test_osc exercises the gated OSC subsystem (OscControl: cue server + outbound).
# test_transports covers the command transports (stream + UDS dgram), which
# build into the SuperSonic exe rather than the shared engine lib.
if(CLOCKWORK_ENABLE_OSC)
    target_sources(SuperSonicNativeTests PRIVATE
        test_osc.cpp
        test_transports.cpp
    )
endif()

# SHM command plane (shm_peer_plane.h): in-process round trips + the
# cross-process crash-safety fuzz (spawns supersonic_test_shm_peer below).
target_sources(SuperSonicNativeTests PRIVATE
    test_shm_peer_plane.cpp
    test_shm_peer_crash.cpp
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

# Golden ring-wire corpus shared with the JS conformance test
# (test/unit/ring_wire_conformance.test.mjs).
target_compile_definitions(SuperSonicNativeTests PRIVATE
    RING_WIRE_FIXTURE="${CMAKE_CURRENT_SOURCE_DIR}/../fixtures/ring_wire.txt"
)

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
    add_executable(supersonic_test_link_peer
        peer/main.cpp
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

# ─── Out-of-process SHM command-plane peer ──────────────────────────────────
# Attaches to the engine's segment and writes /sync frames into the peer
# command ring; test_shm_peer_crash.cpp spawns and SIGKILLs it to prove the
# plane's crash-safety. Header-only dependencies (server_shm + ring writer).
add_executable(supersonic_test_shm_peer
    peer_shm/main.cpp
)
target_include_directories(supersonic_test_shm_peer PRIVATE
    ${SUPERSONIC_ROOT}
    ${SUPERSONIC_ROOT}/clockwork/src
)
# Header-only, so it inherits no usage requirements — including the C++20 the
# headers it includes need (clockwork_path.h uses std::filesystem). State it.
set_target_properties(supersonic_test_shm_peer PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
)
# The peer addresses shm regions by compile-time constants, so it must be built
# with the engine's exact profile or server_shm's layout guard rejects it. Those
# knobs (CLOCKWORK_SHM_AUDIO_SECONDS, NODE_TREE_MIRROR_MAX_NODES, ...) ride on
# clockwork as PUBLIC defs, which the peer can't inherit: it is header-only by
# design and never links clockwork.
#
# Clockwork exports that profile as CLOCKWORK_COMPILE_DEFS — part of its
# interface, beside CLOCKWORK_DSP and the rest.
#
# This said `${CLOCKWORK_COMPILE_DEFS}`, a name set NOWHERE: upstream calls it
# SUPERSONIC_COMPILE_DEFS at top level, and the extraction renamed it and moved
# it inside the submodule. CMake expands an unset variable to nothing and says
# nothing, so the peer was built with NO profile defs at all and every layout
# constant silently took its default — which the shm layout guard then
# (correctly) refused at attach.
if(NOT DEFINED CLOCKWORK_COMPILE_DEFS)
    message(FATAL_ERROR
        "CLOCKWORK_COMPILE_DEFS is not set. The shm peer must be built with "
        "clockwork's compile profile or server_shm's layout guard will reject "
        "it at attach. This is exported by clockwork/CMakeLists.txt.")
endif()
target_compile_definitions(supersonic_test_shm_peer PRIVATE
    ${CLOCKWORK_COMPILE_DEFS}
)
find_package(Threads REQUIRED)
target_link_libraries(supersonic_test_shm_peer PRIVATE Threads::Threads)
if(UNIX AND NOT APPLE)
    target_link_libraries(supersonic_test_shm_peer PRIVATE rt)
endif()
target_compile_definitions(SuperSonicNativeTests PRIVATE
    CLOCKWORK_TEST_SHM_PEER_BINARY="$<TARGET_FILE:supersonic_test_shm_peer>"
)
add_dependencies(SuperSonicNativeTests supersonic_test_shm_peer)

include(CTest)
include(Catch)
catch_discover_tests(SuperSonicNativeTests)

# test_supersonic_binary.cpp spawns the SuperSonic process: build it first.
add_dependencies(SuperSonicNativeTests SuperSonic)
