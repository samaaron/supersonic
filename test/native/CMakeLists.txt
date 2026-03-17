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
    test_boot.cpp
    test_osc_commands.cpp
    test_synthdef.cpp
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
    test_callbacks.cpp
    test_metrics.cpp
    test_node_tree_mirror.cpp
    test_osc_codec.cpp
    test_load_sample.cpp
    test_state_cache.cpp
    test_device_management.cpp
    test_send_reply.cpp
    test_multichannel.cpp
    test_audio_resume.cpp
    test_fuzzy_match.cpp
    test_scheduling_accuracy.cpp
    test_engine_state.cpp
    OscTestUtils.cpp
    EngineFixture.cpp
)

target_link_libraries(SuperSonicNativeTests PRIVATE
    supersonic_engine
    Catch2::Catch2
)

target_include_directories(SuperSonicNativeTests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
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

include(CTest)
include(Catch)
catch_discover_tests(SuperSonicNativeTests)
