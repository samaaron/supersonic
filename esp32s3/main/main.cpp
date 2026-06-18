// SPDX-License-Identifier: MIT OR GPL-3.0-or-later
//
// A minimal public-ABI engine driver (lanes.h only, no GPL-derived code), so the
// file itself is dual-licensed; a binary that links the GPL engine is GPL.
/*
 * main.cpp — ESP32-S3 engine build-smoke (the ESP-IDF analogue of
 * test/freestanding/freestanding_main.cpp).
 *
 * Boots the shared engine on real Xtensa hardware/emulation through the lanes C
 * ABI and ticks a few blocks. Its primary job is to be COMPILED + LINKED for
 * the esp32s3 target in CI: that is what exercises the constraints the desktop
 * freestanding guard cannot reach —
 *   - SCP_TARGET_ESP32: the no-byte-atomics path (supersonic_heap.cpp),
 *   - SC_COLD_BSS -> EXT_RAM_BSS_ATTR placing the ring arena + schedulers in
 *     PSRAM (needs CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY=y),
 *   - the tiered-memory mem_region allocator, Xtensa codegen, IDF headers.
 *
 * If it is ever run (on-device or under an emulator that backs PSRAM), the
 * serial log reports whether the engine booted, ticked, and replied to /status.
 */
#include "lanes/lanes.h"

#include "esp_log.h"

#include <cmath>
#include <cstdint>

static const char* TAG = "ss-smoke";

// Host glue the shared engine expects a host to define (see the "Minimal host"
// contract in lanes.h). No external shm segment on a self-driven device, so the
// engine uses its own ring_buffer_storage arena; the SuperClock composition root
// with the worklet clock (SUPERSONIC_WORKLET_CLOCK, compiled into the supersonic
// component) supplies the clock platform methods.
extern "C" {
void* g_external_shared_memory = nullptr;
}

extern "C" void app_main(void) {
    // Modest World — a build/boot smoke, not a performance run. NRT, no fs.
    SsWorldOptions opts = {};
    opts.num_buffers              = 128;
    opts.max_nodes                = 256;
    opts.max_graph_defs           = 64;
    opts.max_wire_bufs            = 64;
    opts.num_audio_bus_channels   = 64;
    opts.num_input_bus_channels   = 2;
    opts.num_output_bus_channels  = 2;
    opts.num_control_bus_channels = 256;
    opts.buf_length               = 64;
    opts.real_time_memory_size    = 256;  // KB
    opts.num_rgens                = 8;
    opts.load_graph_defs          = 0;    // no filesystem
    opts.verbosity                = 0;
    opts.shared_memory_id         = 0;    // no external shm

    const double sampleRate = 48000.0;
    ss_init(&opts, sampleRate);

    const uint32_t bl = ss_block_size();
    if (bl == 0) {
        ESP_LOGE(TAG, "engine did not boot (block size 0)");
        return;
    }

    // /status — a bare OSC message — drained and replied within ss_tick.
    static const uint8_t status_msg[12] = {
        '/', 's', 't', 'a', 't', 'u', 's', 0, ',', 0, 0, 0,
    };
    ss_ingress_write(status_msg, sizeof(status_msg), /*source_id=*/0);

    // Self-clock like any RTC-less host: base NTP + elapsed samples / rate.
    const double baseNtp = 2208988800.0;
    uint64_t samplePos = 0;
    int replies = 0;

    for (int block = 0; block < 16; ++block) {
        const double ntp = baseNtp + (double)samplePos / sampleRate;
        if (!ss_tick(ntp, opts.num_output_bus_channels, /*in_channels=*/0)) {
            ESP_LOGE(TAG, "ss_tick reported fatal error");
            return;
        }
        const float* out = ss_audio_out();
        for (uint32_t i = 0; i < bl * opts.num_output_bus_channels; ++i) {
            if (!std::isfinite(out[i])) {
                ESP_LOGE(TAG, "non-finite output sample");
                return;
            }
        }
        ss_egress_rt_drain(
            [](void* ctx, uint32_t, uint32_t, const uint8_t*, uint32_t, uint32_t) {
                ++*static_cast<int*>(ctx);
            },
            &replies, /*max_frames=*/0);
        samplePos += bl;
    }

    if (replies < 1) {
        ESP_LOGE(TAG, "no egress reply to /status — ABI path dead");
        return;
    }
    ESP_LOGI(TAG, "ok — booted, ticked 16 blocks, drained %d egress frame(s)", replies);
}
