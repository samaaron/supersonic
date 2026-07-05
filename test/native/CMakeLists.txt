include(FetchContent)

FetchContent_Declare(
    Catch2
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    GIT_TAG        v3.5.2
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(Catch2)
list(APPEND CMAKE_MODULE_PATH "${Catch2_SOURCE_DIR}/extras")

add_executable(SuperSonicNativeTests
    test_main.cpp
    test_platform.cpp
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
    test_config_validation.cpp
    test_engine_lifecycle.cpp
    test_lifecycle_leaks.cpp
    test_callbacks.cpp
    test_metrics.cpp
    test_metrics_shm.cpp
    test_node_tree_mirror.cpp
    test_osc_codec.cpp
    test_load_sample.cpp
    test_state_cache.cpp
    test_device_management.cpp
    test_send_reply.cpp
    test_control_endpoints.cpp
    test_osc_ingress.cpp
    test_multichannel.cpp
    test_audio_resume.cpp
    test_fuzzy_match.cpp
    test_scheduling_accuracy.cpp
    test_engine_state.cpp
    test_cold_swap.cpp
    test_rebuild_contract.cpp
    test_cold_swap_recovery.cpp
    test_cold_swap_scope.cpp
    test_cold_swap_sampleloader.cpp
    test_world_options_layout.cpp
    test_input_channels.cpp
    test_driver_switch.cpp
    test_rapid_switching.cpp
    test_callback_watchdog.cpp
    test_time_source.cpp
    test_device_selection.cpp
    test_aggregate_buffer_clamp.cpp
    test_device_policy.cpp
    test_audio_recovery.cpp
    test_recover_audio.cpp
    test_scope_race.cpp
    test_heap_growth.cpp
    test_scheduler.cpp
    test_ring_buffer_write.cpp
    test_ring_reader.cpp
    test_benchmark.cpp
    test_bundle_sequential.cpp
    test_input_fallback.cpp
    test_no_audio_device.cpp
    test_rt_alloc.cpp
    test_superclock.cpp
    test_event_scheduler.cpp
    test_reply_routing.cpp
    test_midi_clock_out.cpp
    test_in_ring_drain.cpp
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
if(SUPERSONIC_ENABLE_MIDI)
    target_sources(SuperSonicNativeTests PRIVATE test_midi.cpp)
endif()

# test_gamepad exercises the gated gilrs backend (GamepadControl/ss_gamepad).
if(SUPERSONIC_ENABLE_GAMEPAD)
    target_sources(SuperSonicNativeTests PRIVATE test_gamepad.cpp)
endif()

# test_osc exercises the gated OSC subsystem (OscControl: cue server + outbound).
if(SUPERSONIC_ENABLE_OSC)
    target_sources(SuperSonicNativeTests PRIVATE test_osc.cpp)
endif()

target_link_libraries(SuperSonicNativeTests PRIVATE
    supersonic_engine
    Catch2::Catch2
)

target_include_directories(SuperSonicNativeTests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)

# Golden ring-wire corpus shared with the JS conformance test
# (test/unit/ring_wire_conformance.test.mjs).
target_compile_definitions(SuperSonicNativeTests PRIVATE
    RING_WIRE_FIXTURE="${CMAKE_CURRENT_SOURCE_DIR}/../fixtures/ring_wire.txt"
)

target_compile_definitions(SuperSonicNativeTests PRIVATE
    SUPERSONIC_SYNTHDEFS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../../packages/supersonic-scsynth-synthdefs/synthdefs"
    SUPERSONIC_TEST_SYNTHDEFS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../synthdefs"
    SUPERSONIC_SAMPLES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../../packages/supersonic-scsynth-samples/samples"
    JUCE_APPLICATION_NAME_STRING="supersonic-tests"
    JUCE_APPLICATION_VERSION_STRING="0.1.0"
)

if(MSVC)
    target_compile_options(SuperSonicNativeTests PRIVATE /W1 /wd4244 /wd4267 /wd4305 /permissive-)
    target_link_options(SuperSonicNativeTests PRIVATE /FORCE:MULTIPLE)
endif()

# ─── Out-of-process Link peer used by integration tests ────────────────────
# Minimal Link Audio publisher with CLI-configurable channels and
# deterministic audio generators. Tests spawn it via FakeLinkPeerProcess
# and assert on what the engine receives. Only built when Link is on.
if(SUPERSONIC_ENABLE_LINK)
    add_executable(supersonic_test_link_peer
        peer/main.cpp
    )
    target_link_libraries(supersonic_test_link_peer PRIVATE
        Ableton::Link
    )
    target_compile_definitions(SuperSonicNativeTests PRIVATE
        SUPERSONIC_TEST_LINK_PEER_BINARY="$<TARGET_FILE:supersonic_test_link_peer>"
    )
    add_dependencies(SuperSonicNativeTests supersonic_test_link_peer)
endif()

include(CTest)
include(Catch)
catch_discover_tests(SuperSonicNativeTests)
