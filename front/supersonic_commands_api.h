/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (c) 2026 Sam Aaron
 *
 * supersonic_commands_api.h — supersonic::Commands in C, for a client written
 * in another language: a peer on the engine's shared-memory command plane, a
 * binding over clockwork's client ABI. The one front every path to the engine
 * goes through (docs/COMMANDS.md), in the shape the C ABI already speaks:
 * send bytes, poll messages.
 *
 *   SupersonicCommands* f = supersonic_commands_open(client, 1);
 *   supersonic_commands_send(f, osc, n, token);     // 1: the front's, answered here
 *   n = supersonic_commands_poll(f, out, max);      // the engine's replies, and the front's answers
 *   supersonic_commands_close(f);                   // closes `client` too
 */
#pragma once
#include "clockwork_client.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct SupersonicCommands SupersonicCommands;

/* A front over `client` (clockwork_client_open_shm_peer, or an in-process
 * handle), which it owns from here and closes last. `own_threads` non-zero:
 * the front's file work runs on a thread of its own; zero: the caller runs it
 * (supersonic_commands_run_pending), as a host that drives the engine's
 * control pass does. Null for a null client. */
SupersonicCommands* supersonic_commands_open(ClockworkClient* client, int own_threads);

/* One OSC message or bundle from the caller. 1: a verb of the front's, taken
 * and answered through poll; 0: sent to the engine through the handle. */
int supersonic_commands_send(SupersonicCommands* f, const uint8_t* osc, uint32_t bytes, uint32_t token);

/* Up to `max` messages: the engine's replies the front did not keep for
 * itself, then the front's own answers, in order. The bytes are the front's
 * until the next poll. */
uint32_t supersonic_commands_poll(SupersonicCommands* f, ClockworkClientMessage* out, uint32_t max);

/* Without own threads: the file work queued so far, on the calling thread. */
void supersonic_commands_run_pending(SupersonicCommands* f);

void supersonic_commands_close(SupersonicCommands* f);

#ifdef __cplusplus
}
#endif
