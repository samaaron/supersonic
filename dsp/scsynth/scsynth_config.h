// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2025-2026 Sam Aaron
/*
 * scsynth_config.h — the engine's own sizing.
 *
 * These belong to the guest, not the host. A trigger queue, a node-reply
 * queue and a node-ends queue are things scsynth has because scsynth has a
 * node tree, and clockwork has no notion of one. Sizing them in the host
 * would make every guest carry them, which is the coupling dsp_api.h exists
 * to remove.
 *
 * So the guest owns them. Overridable from the build for a constrained target,
 * the same way clockwork's own profile knobs are.
 *
 * Values are upstream SuperSonic's desktop profile; its embedded profile used
 * 64 for the three FIFOs and 2 timelines.
 */
#ifndef SCSYNTH_CONFIG_H
#define SCSYNTH_CONFIG_H

/* /tr trigger queue depth — RT to NRT. */
#ifndef SC_TRIGGERS_FIFO_SIZE
#define SC_TRIGGERS_FIFO_SIZE 1024
#endif

/* /n_set style node replies. */
#ifndef SC_NODE_REPLY_FIFO_SIZE
#define SC_NODE_REPLY_FIFO_SIZE 1024
#endif

/* /n_end notifications. */
#ifndef SC_NODE_ENDS_FIFO_SIZE
#define SC_NODE_ENDS_FIFO_SIZE 1024
#endif


/* ── World sizing ────────────────────────────────────────────────────────────
 *
 * The buffer table, the node pool, the wire pool, the bus arrays: a host
 * chooses these per boot, by name, and scsynth_options.h is the one list of
 * them with their defaults and ranges. Nothing here restates a default.
 */

/* How much the real-time pool grows by when it overflows, in bytes. 0 — the
 * desktop, the NIF and the web — means it does not: a pool that cannot be had
 * at the size asked for fails the boot with a reason rather than playing with
 * less. An embedded build sets a small non-zero value so a pool sized to
 * internal SRAM can spill into bulk memory on demand. */
#ifndef SCSYNTH_RT_POOL_GROWTH_SIZE
#define SCSYNTH_RT_POOL_GROWTH_SIZE 0
#endif

#endif /* SCSYNTH_CONFIG_H */
