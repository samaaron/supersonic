// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025-2026 Sam Aaron
/*
 * /supersonic/piano/wavetable <bufnum>
 *
 * MdaPiano plays from a sample table that no longer ships inside the binary.
 * The table arrives the way any sample does — a file through /b_allocRead
 * natively, a decoded blob through the inbox and /b_allocPtr on the web —
 * and this verb hands the plugin channel 0 of that buffer, converted to the
 * 16-bit integers it reads. One verb, one path, on both platforms; the buffer
 * can be freed afterwards, the plugin keeps its own copy.
 *
 * A buffer shorter than the table the plugin reads is refused with /fail. A
 * bufnum of -1 takes the table away again, and the piano falls silent.
 */
#ifndef SUPERSONIC_PIANO_WAVETABLE_H
#define SUPERSONIC_PIANO_WAVETABLE_H

#include <stddef.h>

struct World;
struct ReplyAddress;

#ifdef __cplusplus
extern "C" {
#endif

int supersonic_meth_piano_wavetable(struct World* inWorld, int inSize, char* inData,
                                    struct ReplyAddress* inReply);

/* Frames in the table the plugin currently plays from; 0 while it has none. */
size_t supersonic_piano_wavetable_frames(void);

/* The plugin's own minimum (MdaUGens.cpp): a shorter table is not a table. */
size_t supersonic_piano_wavetable_min_frames(void);

#ifdef __cplusplus
}
#endif

#endif
