# SuperSonic engine as an ESP-IDF component (libsupersonic).
#
# The lean, self-driven engine — close to what the freestanding CI target builds
# (the shared core, the scsynth-derived DSP/server, oscpack + TLSF), including the
# tiered-memory bits mem_region.cpp + supersonic_heap.cpp the embedded heap uses.
# (The web build compiles a similar set but uses a different heap — no mem_region.)
# The engine sources live in the repo's src/ tree, three levels up from here.
#
# This component is the reusable engine library for ANY ESP-IDF host. The
# build-smoke in ../../main drives it through the lanes C ABI; a real device
# host would link the same component and add its I2S/clock/transport glue.

get_filename_component(SS_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)
set(SS_SRC "${SS_ROOT}/src")

# scsynth server, minus the desktop/WASM-specific entry points and drivers.
file(GLOB SCSYNTH_SERVER "${SS_SRC}/synth/server/*.cpp")
list(FILTER SCSYNTH_SERVER EXCLUDE REGEX
     "SC_.*Plugins\\.cpp$|scsynth_main\\.cpp$|SC_WebAudio\\.cpp$|SC_Wasm\\.cpp$|SC_WasmOscBuilder\\.cpp$")

file(GLOB SCSYNTH_COMMON_CPP "${SS_SRC}/synth/common/*.cpp")
file(GLOB SCSYNTH_COMMON_C   "${SS_SRC}/synth/common/*.c")

# All UGen plugins except MdaUGens (1.1 MB of embedded piano samples).
file(GLOB SCSYNTH_PLUGINS "${SS_SRC}/synth/plugins/*.cpp")
list(FILTER SCSYNTH_PLUGINS EXCLUDE REGEX "MdaUGens\\.cpp$")

set(OSCPACK
    "${SS_SRC}/vendor/oscpack/osc/OscTypes.cpp"
    "${SS_SRC}/vendor/oscpack/osc/OscOutboundPacketStream.cpp"
    "${SS_SRC}/vendor/oscpack/osc/OscReceivedElements.cpp")

idf_component_register(
  SRCS
    # The shared engine boundary: the device host drives the engine through the
    # same ingress/egress/tick C ABI as the web and native hosts, so the component
    # compiles the shared implementation — audio_processor (init_memory +
    # process_audio, which ss_tick wraps), lanes (the ABI over the ring transport),
    # and the schedulers — rather than re-implementing the bundle drain/fire loop.
    "${SS_SRC}/audio_processor.cpp"
    "${SS_SRC}/lanes/lanes.cpp"
    "${SS_SRC}/scheduler/MidiClockOut.cpp"
    "${SS_SRC}/supersonic_heap.cpp"
    "${SS_SRC}/mem_region.cpp"
    "${SS_SRC}/node_tree.cpp"
    "${SS_SRC}/buffer_commands.cpp"
    "${SS_SRC}/SuperClock.cpp"
    # The SuperClock composition root + its self-driven axes. SUPERSONIC_WORKLET_CLOCK
    # (set below) selects the worklet TimeSource (SAB time formula, no wall clock);
    # without SUPERSONIC_LINK the LinkSession/LinkAudioBridge headers are inline
    # no-ops, so no extra TU. process_audio's MidiClockOut path references
    # SuperClock's platform methods (now(), timelineBpm, …), defined across these
    # units, so the link gets real definitions rather than leaning on --gc-sections
    # to drop the device-dead branch.
    "${SS_SRC}/native/SuperClockNative.cpp"
    "${SS_SRC}/native/TimeSource.cpp"
    "${SS_SRC}/native/MidiTimelines.cpp"
    # SUPERSONIC_WORKLET_CLOCK hosts register the inline /clock route
    # (audio_processor's clockCoreRoute), whose verbs live here.
    "${SS_SRC}/EngineClock.cpp"
    "${SS_SRC}/synth/external_libraries/TLSF-2.4.6/src/tlsf.c"
    ${SCSYNTH_SERVER}
    ${SCSYNTH_COMMON_CPP}
    ${SCSYNTH_COMMON_C}
    ${SCSYNTH_PLUGINS}
    # sc3-plugins: Decimator (sample-rate/bit reduction) — sonic-pi-fx_bitcrusher.
    "${SS_SRC}/synth/plugins/sc3-plugins/DistortionUGens.cpp"
    ${OSCPACK}
  INCLUDE_DIRS
    "${SS_SRC}/native/NativeShim"  # emscripten.h / boost stubs for the lean build
    "${SS_SRC}"
    "${SS_SRC}/synth/include/common"
    "${SS_SRC}/synth/include/plugin_interface"
    "${SS_SRC}/synth/include/server"
    "${SS_SRC}/synth/server"
    "${SS_SRC}/synth/common"
    "${SS_SRC}/synth/external_libraries/boost"
    "${SS_SRC}/synth/external_libraries/nova-simd"
    "${SS_SRC}/synth/external_libraries/TLSF-2.4.6/src"
    "${SS_SRC}/vendor/oscpack"
  REQUIRES driver esp_timer)

# Engine feature defines (lean profile — SCP_TARGET_ESP32 is auto-selected from
# ESP_PLATFORM in SC_Platform.h). SUPERSONIC_DEVICE_PROFILE=1 selects the
# ESP32-S3 memory profile in memory_profile.h, which shrinks every compile-time
# region for the device — most importantly static_audio_bus (MAX_CHANNELS 2,
# MAX_BLOCK_SIZE 64 -> 512 B, vs 512 KB on the desktop profile). The sizing knobs
# all live in memory_profile.h, so they are NOT set here (an explicit -D would
# override the profile).
# SUPERSONIC_SYNTH=1 compiles the scsynth engine (World + DSP render); this
# component globs the scsynth sources and main.cpp boots a World, so the synth
# guards in audio_processor.cpp must be on. SUPERSONIC_WORKLET_CLOCK=1 selects the
# self-driven TimeSource (no WallClock/threads on the device).
target_compile_definitions(${COMPONENT_LIB} PRIVATE
    SUPERSONIC=1 STATIC_PLUGINS=1 NOVA_SIMD=1 SC_FFT_GREEN=1 NO_LIBSNDFILE=1
    SC_AUDIO_API=3 SUPERSONIC_DEVICE_PROFILE=1 SUPERSONIC_WORKLET_CLOCK=1
    SUPERSONIC_SYNTH=1)

# The engine is a large C++ codebase — SuperSonic's own core plus the
# scsynth-derived DSP/server and vendored libs (oscpack, TLSF) — and trips many
# GCC warnings IDF promotes to errors (-Werror=format-truncation et al.). Not
# worth chasing in derived/vendored code: disable warnings for the engine
# objects, and keep the DSP math fast.
target_compile_options(${COMPONENT_LIB} PRIVATE -w -fno-math-errno)
