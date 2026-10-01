// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2025-2026 Sam Aaron
/*
 * scsynth_scope.h — the scope entry points, declared where scsynth's types are.
 *
 * Clockwork owns the scope slots and the ring a client reads; a guest reaches
 * them through DspHost::scope_open / scope_write / scope_close (dsp_api.h),
 * and never learns the layout. These four are the guest's own doors to those
 * three, implemented in scsynth_dsp.cpp where the host table lives:
 *
 *   - get / push / release are the shape the InterfaceTable already has
 *     (fGetScopeBuffer, fPushScopeBuffer, fReleaseScopeBuffer), so the ugens
 *     compile unchanged. push is a no-op: a stream publishes on every write.
 *   - write is what a ugen calls per block, with its input buffers.
 *
 * ScopeBufferHnd is scsynth's own (SC_InterfaceTable.h) and is laid out
 * identically to DspScopeHandle: { void*, float*, uint32, uint32 }. After a
 * successful get, internalData is non-null while the slot is held and
 * `channels` is how many channels the slot carries; `data` is null and
 * channel_data() must not be used.
 */
#ifndef SCSYNTH_SCOPE_H
#define SCSYNTH_SCOPE_H

#include <stdint.h>

struct World;
struct ScopeBufferHnd;

#ifdef __cplusplus
extern "C" {
#endif

bool supersonic_scope_get(struct World* world, int index, int channels, int maxFrames,
                          struct ScopeBufferHnd* hnd);
void supersonic_scope_push(struct World* world, struct ScopeBufferHnd* hnd, int frames);
void supersonic_scope_release(struct World* world, struct ScopeBufferHnd* hnd);
void supersonic_scope_write(struct ScopeBufferHnd* hnd, const float* const* channels,
                            uint32_t n_channels, uint32_t frames);

#ifdef __cplusplus
}
#endif

#endif /* SCSYNTH_SCOPE_H */
