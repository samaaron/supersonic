/*
 * nosynth_smoke.cpp — runtime smoke for the no-synth build
 * (SUPERSONIC_ENABLE_SYNTH=OFF). Proves the drivebelt turns the engine's clock
 * with no synth present: ss_tick advances time and the scheduler releases a
 * timed event. (Synth presence is a separate concern — that is the /status
 * egress reply, asserted only in the synth-ON smoke.)
 *
 * Asserts:
 *   1. ss_init + repeated ss_tick run with no World.
 *   2. a #bundle timetagged ahead is held by the scheduler, then fired once
 *      ss_tick passes its timetag — i.e. the clock advanced, the belt turned.
 *   3. an immediate, unrouted message drains without crashing (no backend).
 *
 * NTP base is small (1000 s) so the tick clock and the bundle timetag stay
 * positive and consistently ordered — a real-date NTP overflows the int64 OSC
 * timetag into the negative range.
 */
#include "lanes/lanes.h"
#include "scheduler/EngineScheduler.h"
#include "scheduler/schedule_parse.h"

#include <cstdint>
#include <cstdio>
#include <cstring>

namespace {
void put_be64(uint8_t* p, uint64_t v) {
    for (int i = 0; i < 8; ++i) p[i] = static_cast<uint8_t>((v >> ((7 - i) * 8)) & 0xFF);
}
} // namespace

int main() {
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
    opts.real_time_memory_size    = 256;
    opts.num_rgens                = 8;
    opts.load_graph_defs          = 0;
    opts.verbosity                = 0;
    opts.shared_memory_id         = 0;

    ss_init(&opts, 48000.0);

    const double base = 1000.0;
    const uint32_t out_ch = opts.num_output_bus_channels;

    // (1) The no-synth core must tick without a World.
    for (int i = 0; i < 8; ++i) {
        if (!ss_tick(base, out_ch, 0)) {
            std::fprintf(stderr, "nosynth: ss_tick reported fatal error\n");
            return 1;
        }
    }

    // (2) ingress → drain → schedule. A 16-byte #bundle (header + timetag, no
    // elements) timed 0.5 s ahead.
    uint8_t bundle[16];
    std::memcpy(bundle, "#bundle", 8);  // "#bundle\0"
    put_be64(bundle + 8, static_cast<uint64_t>(ss_ntp_to_timetag(base + 0.5)));
    ss_ingress_write(bundle, sizeof(bundle), /*source_id=*/0);

    ss_tick(base, out_ch, 0);                       // timetag still ahead → held
    if (get_scheduler().size() != 1) {
        std::fprintf(stderr, "nosynth: bundle not scheduled (size=%d)\n",
                     get_scheduler().size());
        return 1;
    }

    ss_tick(base + 1.0, out_ch, 0);                 // past the timetag → fires
    if (get_scheduler().size() != 0) {
        std::fprintf(stderr, "nosynth: scheduled event did not fire (size=%d)\n",
                     get_scheduler().size());
        return 1;
    }

    // (3) An immediate, unrouted message drains without crashing.
    static const uint8_t msg[12] = {'/', 'x', 0, 0, ',', 0, 0, 0, 0, 0, 0, 0};
    ss_ingress_write(msg, sizeof(msg), /*source_id=*/0);
    ss_tick(base + 1.0, out_ch, 0);

    std::printf("nosynth: ok — booted, ticked, scheduled + fired one event, no synth\n");
    return 0;
}
