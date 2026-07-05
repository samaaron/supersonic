/*
 * memory_profile.h — Single source of truth for SuperSonic memory sizing
 *
 * SuperSonic runs on wildly different memory budgets: a desktop/native build
 * with gigabytes of RAM, a WASM AudioWorklet with a managed heap, and
 * embedded targets like the ESP32-S3 with ~512 KiB of fast internal SRAM plus
 * slower PSRAM. Every region that is sized at compile time is collected here so
 * that porting to a new device means writing one small profile block rather
 * than hunting macros across a dozen headers.
 *
 * ── Design rules ────────────────────────────────────────────────────────────
 *   1. This is a PURE-MACRO LEAF header. It includes nothing and defines no
 *      types, so it can be pulled in by the lowest-level headers (including
 *      synth/common/shm_audio_buffer.hpp) without creating dependencies.
 *   2. Every knob is set with an `#ifndef` guard, so precedence is:
 *          explicit -D on the command line   (highest)
 *        > the selected SUPERSONIC_DEVICE_PROFILE block
 *        > the universal defaults at the bottom of this file  (lowest)
 *   3. The universal defaults are the desktop / WASM / NIF / test layout. The JS
 *      side reads the actual layout back from the WASM `get_buffer_layout()`
 *      export, so overriding a size here propagates to JS automatically.
 *
 * ── Adding a device ─────────────────────────────────────────────────────────
 *   Give it a SUPERSONIC_PROFILE_* id, add an `#if SUPERSONIC_DEVICE_PROFILE
 *   == ...` block below that defines whichever knobs differ from the defaults,
 *   then select it from the build with -DSUPERSONIC_DEVICE_PROFILE=<id>.
 *
 * ── The knobs (and where they are consumed) ─────────────────────────────────
 *   SAB / ring-buffer regions ............ shared_memory.h
 *     SUPERSONIC_IN_BUFFER_SIZE            OSC in  (host -> engine)
 *     SUPERSONIC_OUT_BUFFER_SIZE           OSC out (engine -> host)
 *     SUPERSONIC_NRT_OUT_BUFFER_SIZE       NRT-thread egress ring
 *     NODE_TREE_MIRROR_MAX_NODES           node-tree mirror capacity
 *     SHM_SCOPE_MAX_SCOPES                 scope slots
 *     SHM_SCOPE_FRAMES_PER_SCOPE           frames per scope triple-buffer
 *   Scheduler pool ....................... shared_memory.h / scheduler/EngineScheduler.h
 *     SCHEDULER_DATA_POOL_SIZE             bundle data pool bytes
 *     SCHEDULER_SLOT_COUNT                 max scheduled bundles
 *   Notification FIFOs ................... synth/server/SC_HiddenWorld.h
 *     SC_TRIGGERS_FIFO_SIZE               /tr trigger queue depth
 *     SC_NODE_REPLY_FIFO_SIZE             node-reply queue depth
 *     SC_NODE_ENDS_FIFO_SIZE              /n_go,/n_end queue depth
 *   RT heap (AllocPool) .................. supersonic_config.h / supersonic_heap.cpp
 *     SUPERSONIC_HEAP_SIZE                 nominal pool bytes
 *     SUPERSONIC_HEAP_GROWTH_SIZE          growth-area bytes when exhausted (Bulk tier)
 *     SUPERSONIC_HEAP_FAST_SIZE            Fast-tier initial area (== HEAP_SIZE off-device)
 *   Audio graph caps ..................... audio_config.h
 *     SUPERSONIC_MAX_BLOCK_SIZE            static_audio_bus block cap (non-WASM)
 *     SUPERSONIC_DEFAULT_BLOCK_SIZE        default control block size (non-WASM)
 *     SUPERSONIC_MAX_CHANNELS              per-world max channels
 *   Audio capture ring ................... synth/common/shm_audio_buffer.hpp
 *     SUPERSONIC_MAX_SHM_AUDIO_BUFFERS     capture slot count
 *     SUPERSONIC_SHM_AUDIO_SECONDS         per-slot ring duration (seconds)
 *     SUPERSONIC_SHM_AUDIO_SAMPLE_RATE     capture ring sample rate
 *     SUPERSONIC_SHM_AUDIO_FRAMES          per-slot ring frames (overrides s*rate)
 */

#ifndef SUPERSONIC_MEMORY_PROFILE_H
#define SUPERSONIC_MEMORY_PROFILE_H

// ── Device profile selection ────────────────────────────────────────────────
// Numeric ids so they can be compared in the preprocessor.
#define SUPERSONIC_PROFILE_DEFAULT 0
#define SUPERSONIC_PROFILE_ESP32S3 1
#define SUPERSONIC_PROFILE_TEENSY41 2

#ifndef SUPERSONIC_DEVICE_PROFILE
#define SUPERSONIC_DEVICE_PROFILE SUPERSONIC_PROFILE_DEFAULT
#endif

// ── ESP32-S3 profile ────────────────────────────────────────────────────────
// Tight budget for the Waveshare ESP32-S3-Touch-LCD-1.9 (ESP32-S3R8: 512 KiB
// internal SRAM, 8 MiB octal PSRAM, 16 MiB flash) + Pimoroni Pico Audio Pack.
//
// Baseline values, not yet retuned against an on-device build. Each is
// `#ifndef`-guarded so a build can still pin any individual knob with an
// explicit -D.
#if SUPERSONIC_DEVICE_PROFILE == SUPERSONIC_PROFILE_ESP32S3

  #ifndef SUPERSONIC_IN_BUFFER_SIZE
  #define SUPERSONIC_IN_BUFFER_SIZE 32768          // 32 KB
  #endif
  #ifndef SUPERSONIC_OUT_BUFFER_SIZE
  #define SUPERSONIC_OUT_BUFFER_SIZE 8192          // 8 KB
  #endif
  #ifndef SUPERSONIC_NRT_OUT_BUFFER_SIZE
  #define SUPERSONIC_NRT_OUT_BUFFER_SIZE 4096        // 4 KB
  #endif
  #ifndef NODE_TREE_MIRROR_MAX_NODES
  #define NODE_TREE_MIRROR_MAX_NODES 128
  #endif
  #ifndef SHM_SCOPE_MAX_SCOPES
  #define SHM_SCOPE_MAX_SCOPES 1
  #endif
  #ifndef SHM_SCOPE_FRAMES_PER_SCOPE
  #define SHM_SCOPE_FRAMES_PER_SCOPE 128
  #endif
  #ifndef SC_MAX_TIMELINES
  #define SC_MAX_TIMELINES 2
  #endif
  #ifndef SCHEDULER_DATA_POOL_SIZE
  #define SCHEDULER_DATA_POOL_SIZE 65536           // 64 KB
  #endif
  #ifndef SUPERSONIC_HEAP_SIZE
  #define SUPERSONIC_HEAP_SIZE 786432              // 768 KB nominal pool budget
  #endif
  #ifndef SUPERSONIC_HEAP_GROWTH_SIZE
  #define SUPERSONIC_HEAP_GROWTH_SIZE 262144       // 256 KB growth (into Bulk/PSRAM)
  #endif
  #ifndef SUPERSONIC_HEAP_FAST_SIZE
  #define SUPERSONIC_HEAP_FAST_SIZE 81920          // 80 KB internal-SRAM initial area. Sized to
                                                    // hold the World_New hot working set (audio/
                                                    // control buses, per-sample wire scratch,
                                                    // SndBuf arrays) so the render reads SRAM.
                                                    // Measured on the ESP32-S3: 48 KB overflows
                                                    // into PSRAM growth (heapGrow=1) and the
                                                    // render distorts; 64 KB is the heapGrow=0
                                                    // floor; 80 KB adds headroom for larger
                                                    // scenes.
  #endif
  // Internal SRAM to leave free for allocations made after the engine boots: the
  // audio task's stack (32 KB, internal — it runs scsynth's deep render call chain),
  // I2S DMA descriptors, display/touch drivers. The heap and RT-pool initial areas
  // are bounded by largest_free(Fast) minus this reserve; without it they can claim
  // the whole largest block, and the audio task's xTaskCreate then fails and the
  // engine is never ticked.
  #ifndef SUPERSONIC_FAST_RESERVE
  #define SUPERSONIC_FAST_RESERVE 45056            // 44 KB (32 KB audio task stack + I2S DMA + margin)
  #endif
  // RT AllocPool growth-area size (Bulk/PSRAM) when the Fast initial is exhausted.
  // Non-zero => the pool grows into PSRAM on overflow instead of failing /s_new.
  #ifndef SUPERSONIC_RT_POOL_GROWTH_SIZE
  #define SUPERSONIC_RT_POOL_GROWTH_SIZE 65536     // 64 KB
  #endif
  #ifndef SUPERSONIC_MAX_BLOCK_SIZE
  #define SUPERSONIC_MAX_BLOCK_SIZE 64
  #endif
  #ifndef SUPERSONIC_DEFAULT_BLOCK_SIZE
  #define SUPERSONIC_DEFAULT_BLOCK_SIZE 64
  #endif
  #ifndef SUPERSONIC_MAX_CHANNELS
  #define SUPERSONIC_MAX_CHANNELS 2
  #endif
  #ifndef SUPERSONIC_MAX_SHM_AUDIO_BUFFERS
  #define SUPERSONIC_MAX_SHM_AUDIO_BUFFERS 1
  #endif
  #ifndef SUPERSONIC_SHM_AUDIO_FRAMES
  #define SUPERSONIC_SHM_AUDIO_FRAMES 64
  #endif

#endif // SUPERSONIC_PROFILE_ESP32S3

// ── Teensy 4.1 profile ──────────────────────────────────────────────────────
// PJRC Teensy 4.1 (NXP i.MX RT1062: 1 MiB RAM as 512 KiB ITCM/DTCM + 512 KiB
// OCRAM, 8 MiB flash; PSRAM pads unpopulated, so single-tier RAM). The tiered
// knobs (FAST_SIZE/FAST_RESERVE/RT_POOL_GROWTH) stay at their single-region
// defaults. The RT heap is drawn from the OCRAM malloc heap, so
// SUPERSONIC_HEAP_SIZE plus the host's own allocations must fit in ~512 KiB;
// growth is 0 so exhaustion fails the OSC command loudly instead of creeping
// into the audio-library heap. Block size is 128 to match the Teensy audio
// library's per-update frame count (one engine tick per AudioStream update).
// Values retuned against an on-device build.
#if SUPERSONIC_DEVICE_PROFILE == SUPERSONIC_PROFILE_TEENSY41

  #ifndef SUPERSONIC_IN_BUFFER_SIZE
  #define SUPERSONIC_IN_BUFFER_SIZE 32768          // 32 KB
  #endif
  #ifndef SUPERSONIC_OUT_BUFFER_SIZE
  #define SUPERSONIC_OUT_BUFFER_SIZE 8192          // 8 KB
  #endif
  #ifndef SUPERSONIC_NRT_OUT_BUFFER_SIZE
  #define SUPERSONIC_NRT_OUT_BUFFER_SIZE 4096      // 4 KB
  #endif
  #ifndef NODE_TREE_MIRROR_MAX_NODES
  #define NODE_TREE_MIRROR_MAX_NODES 128
  #endif
  #ifndef SHM_SCOPE_MAX_SCOPES
  #define SHM_SCOPE_MAX_SCOPES 1
  #endif
  #ifndef SHM_SCOPE_FRAMES_PER_SCOPE
  #define SHM_SCOPE_FRAMES_PER_SCOPE 128
  #endif
  #ifndef SC_MAX_TIMELINES
  #define SC_MAX_TIMELINES 2
  #endif
  #ifndef SCHEDULER_DATA_POOL_SIZE
  #define SCHEDULER_DATA_POOL_SIZE 65536           // 64 KB
  #endif
  // Notification FIFOs (inside HiddenWorld): 64 deep is ample for a device
  // scene; the desktop 1024s would make sizeof(HiddenWorld) dominate the pool.
  #ifndef SC_TRIGGERS_FIFO_SIZE
  #define SC_TRIGGERS_FIFO_SIZE 64
  #endif
  #ifndef SC_NODE_REPLY_FIFO_SIZE
  #define SC_NODE_REPLY_FIFO_SIZE 64
  #endif
  #ifndef SC_NODE_ENDS_FIFO_SIZE
  #define SC_NODE_ENDS_FIFO_SIZE 64
  #endif
  #ifndef SUPERSONIC_HEAP_SIZE
  #define SUPERSONIC_HEAP_SIZE 98304               // 96 KB engine pool (world buses, SndBufs,
                                                    // wire buffers, /b_alloc). OCRAM (~338 KB
                                                    // after the cold .bss) must also hold the
                                                    // RT pool (host WorldOptions) and the
                                                    // library's registration mallocs.
  #endif
  #ifndef SUPERSONIC_HEAP_GROWTH_SIZE
  #define SUPERSONIC_HEAP_GROWTH_SIZE 0            // no growth: fail loud, keep OCRAM predictable
  #endif
  #ifndef SUPERSONIC_MAX_BLOCK_SIZE
  #define SUPERSONIC_MAX_BLOCK_SIZE 128
  #endif
  #ifndef SUPERSONIC_DEFAULT_BLOCK_SIZE
  #define SUPERSONIC_DEFAULT_BLOCK_SIZE 128
  #endif
  #ifndef SUPERSONIC_MAX_CHANNELS
  #define SUPERSONIC_MAX_CHANNELS 2
  #endif
  #ifndef SUPERSONIC_MAX_SHM_AUDIO_BUFFERS
  #define SUPERSONIC_MAX_SHM_AUDIO_BUFFERS 1
  #endif
  #ifndef SUPERSONIC_SHM_AUDIO_FRAMES
  #define SUPERSONIC_SHM_AUDIO_FRAMES 128
  #endif

#endif // SUPERSONIC_PROFILE_TEENSY41

// ── Universal defaults (desktop / WASM / NIF / tests) ───────────────────────
// Anything left unset by an explicit -D or by the selected profile falls
// through to here.

// SAB / ring-buffer regions
#ifndef SUPERSONIC_IN_BUFFER_SIZE
#define SUPERSONIC_IN_BUFFER_SIZE 786432           // 768 KB
#endif
#ifndef SUPERSONIC_OUT_BUFFER_SIZE
#define SUPERSONIC_OUT_BUFFER_SIZE 131072          // 128 KB
#endif
#ifndef SUPERSONIC_NRT_OUT_BUFFER_SIZE
#define SUPERSONIC_NRT_OUT_BUFFER_SIZE 65536         // 64 KB
#endif
#ifndef NODE_TREE_MIRROR_MAX_NODES
#define NODE_TREE_MIRROR_MAX_NODES 1024
#endif
#ifndef SHM_SCOPE_MAX_SCOPES
#define SHM_SCOPE_MAX_SCOPES 32
#endif
#ifndef SHM_SCOPE_FRAMES_PER_SCOPE
#define SHM_SCOPE_FRAMES_PER_SCOPE 1024
#endif

// Max MIDI-clock follower timelines in the SuperClock registry (slot 0 is
// always Link; slots 1..SC_MAX_TIMELINES are midi:<port> followers).
#ifndef SC_MAX_TIMELINES
#define SC_MAX_TIMELINES 8
#endif

// Scheduler pool
#ifndef SCHEDULER_DATA_POOL_SIZE
#define SCHEDULER_DATA_POOL_SIZE (512 * 1024)      // 512 KB
#endif
#ifndef SCHEDULER_SLOT_COUNT
#define SCHEDULER_SLOT_COUNT 512
#endif

// Notification FIFO depths (SC_HiddenWorld.h). Defaults match upstream scsynth's
// fixed sizes; MsgFifo requires each to be a power of two >= 2.
#ifndef SC_TRIGGERS_FIFO_SIZE
#define SC_TRIGGERS_FIFO_SIZE 1024
#endif
#ifndef SC_NODE_REPLY_FIFO_SIZE
#define SC_NODE_REPLY_FIFO_SIZE 1024
#endif
#ifndef SC_NODE_ENDS_FIFO_SIZE
#define SC_NODE_ENDS_FIFO_SIZE 1024
#endif

// RT heap (AllocPool)
#ifndef SUPERSONIC_HEAP_SIZE
#define SUPERSONIC_HEAP_SIZE (64 * 1024 * 1024)    // 64 MB
#endif
#ifndef SUPERSONIC_HEAP_GROWTH_SIZE
#define SUPERSONIC_HEAP_GROWTH_SIZE (16 * 1024 * 1024) // 16 MB growth area when exhausted
#endif
// The initial pool area drawn from the Fast tier (mem_region.h). On desktop/WASM
// this equals SUPERSONIC_HEAP_SIZE, so the pool is one region and behaviour is
// unchanged; an embedded profile shrinks it so only the boot-time hot set lands
// in fast internal SRAM and later/large buffers grow into Bulk (PSRAM).
#ifndef SUPERSONIC_HEAP_FAST_SIZE
#define SUPERSONIC_HEAP_FAST_SIZE SUPERSONIC_HEAP_SIZE
#endif
// Fast-tier reserve + RT-pool growth. Both 0 on single-tier targets: largest_free()
// returns SIZE_MAX there so the boot-time bounding never clamps, and growth 0 keeps
// the RT pool a fixed region with no allocation after boot (the desktop/NIF shape).
// An embedded profile sets both non-zero.
#ifndef SUPERSONIC_FAST_RESERVE
#define SUPERSONIC_FAST_RESERVE 0
#endif
#ifndef SUPERSONIC_RT_POOL_GROWTH_SIZE
#define SUPERSONIC_RT_POOL_GROWTH_SIZE 0
#endif

// Audio graph caps (non-WASM; the WASM render quantum is fixed at 128)
#ifndef SUPERSONIC_MAX_BLOCK_SIZE
#define SUPERSONIC_MAX_BLOCK_SIZE 1024
#endif
#ifndef SUPERSONIC_DEFAULT_BLOCK_SIZE
#define SUPERSONIC_DEFAULT_BLOCK_SIZE 128
#endif
#ifndef SUPERSONIC_MAX_CHANNELS
#define SUPERSONIC_MAX_CHANNELS 128
#endif

// Audio capture ring (shm_audio_buffer)
#ifndef SUPERSONIC_MAX_SHM_AUDIO_BUFFERS
#define SUPERSONIC_MAX_SHM_AUDIO_BUFFERS 4
#endif
#ifndef SUPERSONIC_SHM_AUDIO_SECONDS
#define SUPERSONIC_SHM_AUDIO_SECONDS 1
#endif
#ifndef SUPERSONIC_SHM_AUDIO_SAMPLE_RATE
#define SUPERSONIC_SHM_AUDIO_SAMPLE_RATE 48000
#endif
// Per-slot frames default to seconds * sample-rate; a profile may override the
// frame count directly (e.g. to shrink below one second, which the seconds knob
// can't express because it is an integer).
#ifndef SUPERSONIC_SHM_AUDIO_FRAMES
#define SUPERSONIC_SHM_AUDIO_FRAMES \
    (SUPERSONIC_SHM_AUDIO_SAMPLE_RATE * SUPERSONIC_SHM_AUDIO_SECONDS)
#endif

#endif // SUPERSONIC_MEMORY_PROFILE_H
