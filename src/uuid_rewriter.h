/*
    SuperSonic - UUID ↔ int32 rewriter for scsynth compatibility
    Copyright (c) 2025 Sam Aaron

    Rewrites UUID node IDs (OSC type tag 'u', 16 bytes) to int32 ('i', 4 bytes)
    before scsynth processes them, and reverse-maps int32 back to UUID in replies.

    Zero-allocation design: two static hash tables in WASM data segment (~192KB).
    Open-addressing with linear probing, backward-shift deletion (Knuth Algorithm R).
    No tombstones — probe chains stay clean over long sessions.

    Called from process_audio() (outbound) and osc_reply_to_ring_buffer() (inbound).

    GPL v3 or later
*/

#ifndef SUPERSONIC_UUID_REWRITER_H
#define SUPERSONIC_UUID_REWRITER_H

#include <cstdint>
#include <atomic>

// Hash table capacity for forward and reverse tables (must be power of 2).
// At ~50% load, supports up to 2048 concurrent UUID-mapped synths.
constexpr uint32_t UUID_HASH_CAPACITY = 4096;
constexpr uint32_t UUID_HASH_MASK = UUID_HASH_CAPACITY - 1;

// Initialize the rewriter with a pointer to the shared NODE_ID_COUNTER
void uuid_rewriter_init(uint8_t* shared_memory, uint32_t node_id_counter_start);

// Outbound: rewrite 'u' type tags to 'i' in-place (messages only shrink)
// Returns true if any rewriting occurred
bool rewrite_uuid_to_int32(char* osc_data, uint32_t* payload_size);

// Inbound: rewrite int32 node IDs back to UUIDs for known reply addresses
// Writes expanded message to out_buf, sets *out_size.
// If no rewriting needed, *out_size == size (caller should use original msg).
void rewrite_int32_to_uuid(const char* msg, int size, char* out_buf, int* out_size);

// Diagnostics
uint32_t uuid_map_get_count();
uint32_t uuid_map_get_capacity();

// Lookup UUID by int32 node_id. Returns true if found, writes uuid_hi/uuid_lo.
bool uuid_map_reverse_lookup(int32_t node_id, uint64_t* out_hi, uint64_t* out_lo);

// Lookup int32 by UUID. Returns node_id or -1 if not found.
int32_t uuid_map_forward_lookup(uint64_t uuid_hi, uint64_t uuid_lo);

#endif // SUPERSONIC_UUID_REWRITER_H
