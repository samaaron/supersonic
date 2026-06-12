/*
    SuperSonic - SuperCollider AudioWorklet WebAssembly port
    Copyright (c) 2025 Sam Aaron

    Based on SuperCollider by James McCartney and community
    GPL v3 or later
*/

/*
 * lanes_internal.h — engine-internal lanes plumbing (not host API).
 *
 * ss_egress_nrt_write: the NRT egress *producer*. Engine control-thread
 * subsystems (the OscEgress framer; embedded MIDI/Link tasks) frame
 * [route][osc] into the NRT-out ring through here. It is NOT part of the host
 * ABI — hosts only ever drain egress (ss_egress_nrt_drain). It lives here, not
 * in lanes.h, so the public boundary stays strictly write-ingress /
 * read-egress / tick.
 *
 * ss_lanes_reset_drains: called by init_memory() when the arena (and the ring
 * sequence counters) are reset, so the egress drains' process-lifetime
 * consumer state (sequence-gap tracking) restarts with them — otherwise a
 * second engine boot in the same process counts a spurious sequence gap.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* token is the origin sourceId the frame replies to (broadcasts ignore it). */
bool ss_egress_nrt_write(uint32_t route, uint32_t token,
                         const uint8_t* osc, uint32_t len);

void ss_lanes_reset_drains(void);

#ifdef __cplusplus
}
#endif
