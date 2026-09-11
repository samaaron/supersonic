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

#ifndef SC_MAX_TIMELINES
#define SC_MAX_TIMELINES 8
#endif

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
 * These eight numbers used to arrive from the host, through the options block
 * and across the seam. They do not any more, deliberately: they size scsynth's
 * INTERNAL tables — its buffer table, its node pool, its wire pool, its bus
 * arrays — and a host cannot know the right values for a graph it cannot see.
 * dsp_api.h passes what only the host can know (rate, block, channel counts)
 * and stops.
 *
 * So the guest picks them, and a build can override any of them. Values are
 * upstream SuperSonic's defaults.
 */
#ifndef SC_NUM_BUFFERS
#define SC_NUM_BUFFERS 1024
#endif
#ifndef SC_MAX_NODES
#define SC_MAX_NODES 1024
#endif
#ifndef SC_MAX_GRAPH_DEFS
#define SC_MAX_GRAPH_DEFS 1024
#endif
#ifndef SC_MAX_WIRE_BUFS
#define SC_MAX_WIRE_BUFS 64
#endif
#ifndef SC_NUM_AUDIO_BUS_CHANNELS
#define SC_NUM_AUDIO_BUS_CHANNELS 1024
#endif
#ifndef SC_NUM_CONTROL_BUS_CHANNELS
#define SC_NUM_CONTROL_BUS_CHANNELS 16384
#endif
#ifndef SC_REAL_TIME_MEMORY_SIZE
#define SC_REAL_TIME_MEMORY_SIZE 8192   /* KB */
#endif
#ifndef SC_NUM_RGENS
#define SC_NUM_RGENS 64
#endif

#endif /* SCSYNTH_CONFIG_H */
