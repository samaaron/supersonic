#pragma once
/*
 * audio_config.h
 *
 * Platform-specific audio configuration caps. Keeps the 128-sample block
 * size pin on the web build (AudioWorklet's fixed render quantum) while
 * letting native pick a block size that matches the hardware callback.
 *
 * All runtime code should read the actual block size from
 * g_world->mBufLength; these constants only size static buffers (like
 * static_audio_bus) which need a compile-time max.
 */

#include <cstdio>
#include <cstdlib>

#include "memory_profile.h"

namespace sonicpi {

// Runtime-gated deep diagnostic logging. A few hard-to-reach issues
// (e.g. scsynth scope-buffer lifecycle corruption) need more detail
// than we want polluting normal operation. Users / we can enable
// this for a specific bug report by launching with SONICPI_DEV_LOG=1
// in the environment — no rebuild needed.
//
// Evaluated once on first call and cached in a function-local static,
// so the hot-path cost is a single atomic load after initialisation.
// Compile-time gating would save even that but forces a rebuild for
// support requests, which is a worse tradeoff.
inline bool devLogEnabled() {
    static const bool enabled = []() {
        const char* v = std::getenv("SONICPI_DEV_LOG");
        return v != nullptr && v[0] != '\0' && v[0] != '0';
    }();
    return enabled;
}

#ifdef __EMSCRIPTEN__
// AudioWorklet render quantum is fixed at 128 samples by the Web Audio
// spec. Block size must equal render quantum exactly.
inline constexpr int kMaxBlockSize     = 128;
inline constexpr int kDefaultBlockSize = 128;
#else
// Non-WASM build — block size comes from memory_profile.h (default max 1024,
// which covers every HW buffer we've seen on macOS/Windows/Linux drivers,
// typically 64–512). Embedded profiles shrink this hard: static_audio_bus
// costs kMaxBlockSize * kMaxChannels * 4 B in .bss, so 1024 × 128 = 512 KB on
// desktop but e.g. 64 × 2 = 512 B on the ESP32-S3 profile.
inline constexpr int kMaxBlockSize     = SUPERSONIC_MAX_BLOCK_SIZE;
inline constexpr int kDefaultBlockSize = SUPERSONIC_DEFAULT_BLOCK_SIZE;
#endif

// Scsynth's per-world max output channels (default 128); sized per device via
// memory_profile.h. Not platform-dependent.
inline constexpr int kMaxChannels = SUPERSONIC_MAX_CHANNELS;

// Named indices into the uint32_t opts[] array at
// ring_buffer_storage + WORLD_OPTIONS_START. audio_processor.cpp
// reads these when building a WorldOptions struct for init_memory() /
// World_New(); SupersonicEngine and JuceAudioCallback write them.
// Previously accessed as raw magic indices (opts[5], opts[6], opts[14]
// etc) with sprinkled comments — easy to miscount when touching the
// runtime hot-update sites in switchDevice / enableInputChannels.
namespace WorldOpts {
    enum : unsigned {
        kNumBuffers            =  0,
        kMaxNodes              =  1,
        kMaxGraphDefs          =  2,
        kMaxWireBufs           =  3,
        kNumAudioBusChannels   =  4,
        kNumInputBusChannels   =  5,
        kNumOutputBusChannels  =  6,
        kNumControlBusChannels =  7,
        kBufLength             =  8,
        kRealTimeMemorySize    =  9,
        kNumRGens              = 10,
        kRealTime              = 11,
        kMemoryLocking         = 12,
        kLoadGraphDefs         = 13,
        kSampleRate            = 14,
        kVerbosity             = 15,
        // Index 16 onwards differs by runtime, so it carries runtime-specific
        // names rather than one shared one:
        //   16  web:    RT pool byte offset in the SAB (writeWorldOptionsToMemory
        //               → init_memory's __EMSCRIPTEN__ branch). No native meaning;
        //               ss_init writes an explicit 0 (struct hosts have no
        //               external pool).
        //   17  native: sharedMemoryID; web: transport flag (0 = SAB, 1 = PM).
        // Each side MUST read/write at the SAME index — a native write at 17
        // read back at 18 silently picked up the next region's first word
        // (scope maxScopes) and made the World create a stray shm segment
        // every boot; and index 16 once carried a name (kMode) that nothing
        // read, hiding that ss_init's zero-write shares the slot with the
        // web's RT pool offset. Name every divergent slot to keep the sides
        // locked (test/world_options_contract.spec.mjs enforces this).
        kWebRtPoolOffset       = 16,  // web only (JS write + init_memory read)
        kNativeSharedMemoryID  = 17,  // native write (JuceAudioCallback) + native read (init_memory)
        kWebTransportFlag      = 17,  // web only (init_memory, __EMSCRIPTEN__)
    };
}

} // namespace sonicpi

#define DEV_LOG(fmt, ...) \
    do { if (sonicpi::devLogEnabled()) { \
        fprintf(stderr, fmt, ##__VA_ARGS__); fflush(stderr); \
    } } while (0)
