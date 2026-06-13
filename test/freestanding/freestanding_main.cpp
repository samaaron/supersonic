/*
 * freestanding_main.cpp — the "engine builds and runs for ANY host" guard.
 *
 * Compiles and links the shared engine (audio_processor + lanes + the
 * schedulers + scsynth + oscpack) with NO JUCE and NO Emscripten — only the
 * compile-shim headers — and drives it purely through the lanes C ABI:
 * ss_init → ss_ingress_write → ss_tick → ss_egress_rt_drain → ss_audio_out.
 *
 * This is the desktop stand-in for an embedded host: it supplies the one piece
 * of host glue the engine needs (a null external-shared-memory pointer) and
 * shares the lean SuperClockLean.cpp for the clock platform methods. Its job in
 * CI is to fail the moment a JUCE/Emscripten dependency
 * leaks into the shared engine, the lanes ABI breaks, or a shared symbol stops
 * resolving without a full host — i.e. it makes "one C ABI for all build
 * targets" a tested invariant, catchable on a normal runner with no ESP or
 * browser toolchain.
 *
 * Exit 0 = the engine booted and ticked silently for a few blocks. Any compile
 * error, link error, or non-finite output fails the build/run.
 */
#include "lanes/lanes.h"

#include <cmath>
#include <cstdint>
#include <cstdio>

// ── Host glue the shared engine expects a host to provide ────────────────────
// On native this is SupersonicEngine.cpp; embedded defines it too. The
// freestanding host has no public shm segment, so the engine uses its own
// ring_buffer_storage arena. (The SuperClock platform methods — including
// state() — come from the shared lean SuperClockLean.cpp this target compiles;
// g_active_superclock stays unset, so the engine's clock/MIDI paths are skipped.)
extern "C" {
void* g_external_shared_memory = nullptr;
}

int main() {
    // Modest desktop-sized World — this is a build/boot smoke test, not a
    // performance run. NRT/no-filesystem, like every self-driven host.
    SsWorldOptions opts = {};
    opts.num_buffers             = 128;
    opts.max_nodes               = 256;
    opts.max_graph_defs          = 64;
    opts.max_wire_bufs           = 64;
    opts.num_audio_bus_channels  = 64;
    opts.num_input_bus_channels  = 2;
    opts.num_output_bus_channels = 2;
    opts.num_control_bus_channels = 256;
    opts.buf_length              = 64;
    opts.real_time_memory_size   = 256;  // KB
    opts.num_rgens               = 8;
    opts.load_graph_defs         = 0;    // no filesystem
    opts.verbosity               = 0;
    opts.shared_memory_id        = 0;    // no boost/POSIX shm

    const double sampleRate = 48000.0;
    ss_init(&opts, sampleRate);

    const uint32_t bl = ss_block_size();
    if (bl == 0) {
        std::fprintf(stderr, "freestanding: engine did not boot (block size 0)\n");
        return 1;
    }

    // Self-clock like an embedded host: base NTP + elapsed samples / rate.
    const double baseNtp = 2208988800.0;  // arbitrary base (no RTC)
    uint64_t samplePos = 0;

    // /status — a bare OSC message — drained and replied within ss_tick.
    static const uint8_t status_msg[12] = {
        '/', 's', 't', 'a', 't', 'u', 's', 0, ',', 0, 0, 0,
    };
    ss_ingress_write(status_msg, sizeof(status_msg), /*source_id=*/0);

    // Tick a handful of blocks; assert the rendered output is finite.
    int replies = 0;
    for (int block = 0; block < 16; ++block) {
        const double ntp = baseNtp + (double)samplePos / sampleRate;
        if (!ss_tick(ntp, opts.num_output_bus_channels, /*in_channels=*/0)) {
            std::fprintf(stderr, "freestanding: ss_tick reported fatal error\n");
            return 1;
        }

        const float* out = ss_audio_out();
        for (uint32_t i = 0; i < bl * opts.num_output_bus_channels; ++i) {
            if (!std::isfinite(out[i])) {
                std::fprintf(stderr, "freestanding: non-finite output sample\n");
                return 1;
            }
        }

        ss_egress_rt_drain(
            [](void* ctx, uint32_t, uint32_t, const uint8_t*, uint32_t, uint32_t) {
                ++*static_cast<int*>(ctx);
            },
            &replies, /*max_frames=*/0);

        samplePos += bl;
    }

    // The /status round-trip is the actual ABI exercise: ingress → engine →
    // egress. A live engine replies with /status.reply, so zero frames means
    // the ingress classify or egress drain path is broken — fail, don't just log.
    if (replies < 1) {
        std::fprintf(stderr, "freestanding: no egress reply to /status — ABI path dead\n");
        return 1;
    }

    std::printf("freestanding: ok — booted, ticked 16 blocks, drained %d egress frame(s)\n",
                replies);
    return 0;
}
