// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2025-2026 Sam Aaron
/*
 * scsynth_scope.h — the scope entry points, declared where scsynth's types are.
 *
 * Clockwork implements all four (rust/clockwork-scope) and declares only one of
 * them in src/scope_streams.h: clockwork_scope_geometry, whose signature is plain
 * sizes. The other three take a World* and a ScopeBufferHnd*, and clockwork
 * has no World — so declaring them there would have put a scsynth type in a
 * clockwork header, which is exactly the coupling the seam removed.
 *
 * The Rust side takes `*mut c_void` for the world and never dereferences it,
 * so nothing about scsynth reaches clockwork. The type only matters HERE,
 * where scsynth assigns these to its own function-pointer table, and so the
 * declaration belongs here too.
 *
 * ScopeBufferHnd is scsynth's own (SC_Types.h) and is laid out identically to
 * clockwork's; both are { void*, float*, uint32, uint32 }.
 */
#ifndef SCSYNTH_SCOPE_H
#define SCSYNTH_SCOPE_H

struct World;
struct ScopeBufferHnd;

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Returns bool, matching scsynth's SCBool function-pointer table — which is
 * what upstream SuperSonic declared too. The Rust implementation returns a
 * c_int of 0 or 1, so the low byte the caller reads is always right; the
 * mismatch is real but harmless on every ABI in use, and narrowing it here
 * rather than widening scsynth's table is the smaller change.
 */
bool clockwork_scope_get(struct World* world, int index, int channels, int maxFrames,
                   struct ScopeBufferHnd* hnd);
void clockwork_scope_push(struct World* world, struct ScopeBufferHnd* hnd, int frames);
void clockwork_scope_release(struct World* world, struct ScopeBufferHnd* hnd);

#ifdef __cplusplus
}
#endif

#endif /* SCSYNTH_SCOPE_H */
