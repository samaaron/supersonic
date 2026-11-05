// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2025-2026 Sam Aaron
/*
 * guest_doors.h — everything the server wants from its host, by World.
 *
 * scsynth_dsp.cpp builds one instance of the guest per dsp_new and holds the
 * host's table (DspHost), the memory the host handed over and the clock it
 * publishes. The server's own sources never see that instance: they hold a
 * World, and every door here takes the World and finds the instance it
 * belongs to (HiddenWorld::mGuest). Nothing is kept process-wide, so any
 * number of instances can share one process.
 *
 * All of them are implemented in scsynth_dsp.cpp. The server names no
 * clockwork symbol of its own (scripts/check-guest-boundary.sh).
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

struct World;
struct ClockworkClockState;
struct Dsp;
// One instance of the guest: what scsynth_dsp.cpp makes at dsp_new and a
// World belongs to. Opaque to the server.
struct SupersonicGuest;

#ifdef __cplusplus
extern "C" {
#endif

// The World of an instance clockwork holds (the struct Dsp* dsp_new returned),
// for a test that drives the doors directly.
struct World* supersonic_guest_world(struct Dsp* instance);

// The guest's heap: clockwork's heap when there is a host, nova's aligned
// malloc when there is none. Everything the server makes while sound plays —
// a buffer's samples, a synth definition's tables, the piano wavetable — is
// kept here and never with new or malloc, which clockwork keeps off the
// audio thread. 64-byte aligned, as nova-simd's aligned vector loads need
// (32 with AVX); NULL for 0 bytes, or when the heap is out. A free of a range
// the client owns (one inside the assets area) is nothing.
void*       supersonic_guest_alloc_bytes(struct World* world, size_t bytes);
void        supersonic_guest_free_bytes(struct World* world, void* ptr);
// The same heap, for the two blocks that exist before a World does: the
// World and its HiddenWorld (World_New, World_Cleanup).
void*       supersonic_guest_alloc_bytes_for(struct SupersonicGuest* guest, size_t bytes);
void        supersonic_guest_free_bytes_for(struct SupersonicGuest* guest, void* ptr);

// The guest's working memory (DspConfig::memory): clockwork reserved it for
// the pool (dsp_memory_bytes), and the pool is carved from it when it fits.
const void* supersonic_guest_memory(struct World* world, uint32_t* bytes);

// One line of diagnostics to the host of the World's instance, finished text
// in. `len` bounds a buffer that need not be terminated.
void        supersonic_guest_log(struct World* world, const char* text, uint32_t len);

// A buffer bound to an asset is letting go: the client reclaims the bytes
// once clockwork tells it the asset is released (dsp_api.h, "Assets").
void        supersonic_guest_release_buffer(struct World* world, int bufnum);

// Where the assets area is and how big, set at dsp_new. /b_allocPtr names a
// sample by offset from here. contains: non-zero when `ptr` lies inside it, a
// range the client owns, which no free on this side may touch.
const uint8_t* supersonic_assets_base(struct World* world);
uint32_t       supersonic_assets_bytes(struct World* world);
int            supersonic_assets_contains(struct World* world, const void* ptr);
// The returns area likewise: the lane the guest writes and a client reads in place.
uint8_t*       supersonic_returns_base(struct World* world);
uint32_t       supersonic_returns_bytes(struct World* world);

// scsynth's /clearSched, in a server whose scheduler is clockwork's: the
// bundles clockwork holds for this guest ahead of their time are dropped
// (DspHost::flush_schedule).
void        supersonic_flush_schedule(struct World* world);

// The session clock clockwork publishes (DspConfig::clock), and the time of
// the block being rendered as NTP seconds, for the clock UGens.
const struct ClockworkClockState* supersonic_scsynth_session_clock(struct World* world);
double      supersonic_scsynth_block_ntp(struct World* world);

// ScopeOut2's defence against a slot released under it mid-life: the trip
// itself, and how often it has tripped in this instance (the race test reads
// it).
void     supersonic_scope_defence_tripped(struct World* world);
uint32_t supersonic_scope_defence_trips(struct World* world);

// MdaPiano's sample table: what the instance plays from, or NULL while it
// has none. `frames` (optional) is its length.
const short* supersonic_piano_wavetable(struct World* world, size_t* frames);
// Make `fresh` the table (NULL takes it away); the table it replaced comes
// back, for the caller to free once nobody reads it.
short*       supersonic_piano_wavetable_swap(struct World* world, short* fresh, size_t frames);

#ifdef __cplusplus
}
#endif
