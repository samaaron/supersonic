/*
    SuperSonic - UUID ↔ int32 rewriter implementation
    Copyright (c) 2025 Sam Aaron

    Two hash tables (forward: UUID→int32, reverse: int32→UUID) with open-addressing,
    linear probing, and backward-shift deletion (Knuth Algorithm R). No tombstones.
    Matches the pattern used by node_tree.cpp for the scsynth node tree mirror.

    GPL v3 or later
*/

#include "uuid_rewriter.h"
#include <cstring>
#include <climits>

// Forward declaration for debug logging
extern "C" int worklet_debug(const char* fmt, ...);

// =============================================================================
// FORWARD HASH TABLE — UUID → int32
// =============================================================================
// Empty sentinel: node_id == -1 (allocated node IDs start at 1000, never -1).
// No sentinel on UUID bytes — any 128-bit value is a valid key.

constexpr int32_t FWD_EMPTY = -1;

struct FwdEntry {
    uint64_t uuid_hi;
    uint64_t uuid_lo;
    int32_t  node_id;    // FWD_EMPTY = slot is empty
    uint32_t _pad;
};  // 24 bytes per entry, 4096 entries = 96KB

static FwdEntry fwd_hash[UUID_HASH_CAPACITY];

// =============================================================================
// REVERSE HASH TABLE — int32 → UUID
// =============================================================================
// Empty sentinel: node_id == INT32_MIN (matching node_tree.cpp pattern).

constexpr int32_t REV_EMPTY = INT32_MIN;

struct RevEntry {
    int32_t  node_id;    // REV_EMPTY = slot is empty
    uint32_t _pad;
    uint64_t uuid_hi;
    uint64_t uuid_lo;
};  // 24 bytes per entry, 4096 entries = 96KB

static RevEntry rev_hash[UUID_HASH_CAPACITY];

// Active entry count (across both tables, which are always in sync)
static uint32_t uuid_map_count = 0;

// Pointer to shared NODE_ID_COUNTER atomic (set by uuid_rewriter_init)
static std::atomic<int32_t>* node_id_counter = nullptr;

// =============================================================================
// HASH FUNCTIONS
// =============================================================================

static inline uint32_t fwd_hash_func(uint64_t hi, uint64_t lo) {
    return (uint32_t)((hi ^ (hi >> 32) ^ lo ^ (lo >> 32)) & UUID_HASH_MASK);
}

// Murmurhash-style integer hash (matching node_tree.cpp)
static inline uint32_t rev_hash_func(int32_t key) {
    uint32_t h = static_cast<uint32_t>(key);
    h ^= h >> 16;
    h *= 0x45d9f3b;
    h ^= h >> 16;
    return h & UUID_HASH_MASK;
}

// =============================================================================
// FORWARD TABLE OPERATIONS
// =============================================================================

static int32_t fwd_find(uint64_t hi, uint64_t lo) {
    uint32_t idx = fwd_hash_func(hi, lo);
    while (fwd_hash[idx].node_id != FWD_EMPTY) {
        if (fwd_hash[idx].uuid_hi == hi && fwd_hash[idx].uuid_lo == lo) {
            return fwd_hash[idx].node_id;
        }
        idx = (idx + 1) & UUID_HASH_MASK;
    }
    return -1;
}

// Backward-shift deletion (Knuth Algorithm R) — no tombstones
static void fwd_remove(uint64_t hi, uint64_t lo) {
    uint32_t i = fwd_hash_func(hi, lo);
    while (fwd_hash[i].node_id != FWD_EMPTY) {
        if (fwd_hash[i].uuid_hi == hi && fwd_hash[i].uuid_lo == lo) {
            for (;;) {
                fwd_hash[i].node_id = FWD_EMPTY;
                uint32_t j = i;
                for (;;) {
                    j = (j + 1) & UUID_HASH_MASK;
                    if (fwd_hash[j].node_id == FWD_EMPTY) return;
                    uint32_t r = fwd_hash_func(fwd_hash[j].uuid_hi, fwd_hash[j].uuid_lo);
                    if (i <= j) {
                        if (i < r && r <= j) continue;
                    } else {
                        if (i < r || r <= j) continue;
                    }
                    break;
                }
                fwd_hash[i] = fwd_hash[j];
                i = j;
            }
        }
        i = (i + 1) & UUID_HASH_MASK;
    }
}

// =============================================================================
// REVERSE TABLE OPERATIONS
// =============================================================================

static bool rev_find(int32_t node_id, uint64_t* out_hi, uint64_t* out_lo) {
    uint32_t idx = rev_hash_func(node_id);
    while (rev_hash[idx].node_id != REV_EMPTY) {
        if (rev_hash[idx].node_id == node_id) {
            *out_hi = rev_hash[idx].uuid_hi;
            *out_lo = rev_hash[idx].uuid_lo;
            return true;
        }
        idx = (idx + 1) & UUID_HASH_MASK;
    }
    return false;
}

static void rev_insert(int32_t node_id, uint64_t hi, uint64_t lo) {
    uint32_t idx = rev_hash_func(node_id);
    while (rev_hash[idx].node_id != REV_EMPTY) {
        idx = (idx + 1) & UUID_HASH_MASK;
    }
    rev_hash[idx].node_id = node_id;
    rev_hash[idx].uuid_hi = hi;
    rev_hash[idx].uuid_lo = lo;
}

// Backward-shift deletion (Knuth Algorithm R) — no tombstones
static void rev_remove(int32_t node_id) {
    uint32_t i = rev_hash_func(node_id);
    while (rev_hash[i].node_id != REV_EMPTY) {
        if (rev_hash[i].node_id == node_id) {
            for (;;) {
                rev_hash[i].node_id = REV_EMPTY;
                uint32_t j = i;
                for (;;) {
                    j = (j + 1) & UUID_HASH_MASK;
                    if (rev_hash[j].node_id == REV_EMPTY) return;
                    uint32_t r = rev_hash_func(rev_hash[j].node_id);
                    if (i <= j) {
                        if (i < r && r <= j) continue;
                    } else {
                        if (i < r || r <= j) continue;
                    }
                    break;
                }
                rev_hash[i] = rev_hash[j];
                i = j;
            }
        }
        i = (i + 1) & UUID_HASH_MASK;
    }
}

// =============================================================================
// COMBINED OPERATIONS
// =============================================================================

// Insert UUID → int32 mapping. Returns existing node_id if already mapped,
// otherwise allocates a new int32 from the shared counter.
// Single traversal of forward table: finds match or insertion slot in one pass.
static int32_t uuid_insert(uint64_t hi, uint64_t lo) {
    uint32_t idx = fwd_hash_func(hi, lo);
    while (fwd_hash[idx].node_id != FWD_EMPTY) {
        if (fwd_hash[idx].uuid_hi == hi && fwd_hash[idx].uuid_lo == lo) {
            return fwd_hash[idx].node_id;  // Already mapped
        }
        idx = (idx + 1) & UUID_HASH_MASK;
    }

    // Not found — allocate new node ID and insert into both tables
    int32_t new_id = node_id_counter->fetch_add(1, std::memory_order_relaxed);

    if (uuid_map_count > (UUID_HASH_CAPACITY * 3 / 4)) {
        worklet_debug("WARNING: UUID map >75%% full (%u/%u)", uuid_map_count, UUID_HASH_CAPACITY);
    }

    fwd_hash[idx].uuid_hi = hi;
    fwd_hash[idx].uuid_lo = lo;
    fwd_hash[idx].node_id = new_id;

    rev_insert(new_id, hi, lo);
    uuid_map_count++;
    return new_id;
}

// Delete mapping by int32 node_id (called on /n_end).
// O(1) reverse lookup to find UUID, then removes from both tables.
static void uuid_delete_by_id(int32_t node_id) {
    uint64_t hi, lo;
    if (!rev_find(node_id, &hi, &lo)) return;

    rev_remove(node_id);
    fwd_remove(hi, lo);
    if (uuid_map_count > 0) uuid_map_count--;
}

// =============================================================================
// INIT
// =============================================================================

void uuid_rewriter_init(uint8_t* shared_memory, uint32_t node_id_counter_start) {
    node_id_counter = reinterpret_cast<std::atomic<int32_t>*>(
        shared_memory + node_id_counter_start
    );
    // Clear both tables — FWD_EMPTY is -1 so we must set explicitly
    for (uint32_t i = 0; i < UUID_HASH_CAPACITY; i++) {
        fwd_hash[i].node_id = FWD_EMPTY;
    }
    // REV_EMPTY is INT32_MIN — also set explicitly
    for (uint32_t i = 0; i < UUID_HASH_CAPACITY; i++) {
        rev_hash[i].node_id = REV_EMPTY;
    }
    uuid_map_count = 0;
}

// =============================================================================
// OSC PARSING HELPERS
// =============================================================================

// Read a null-terminated, 4-byte-padded OSC string. Returns offset past the padding.
static uint32_t osc_string_end(const char* data, uint32_t pos, uint32_t size) {
    while (pos < size && data[pos] != '\0') pos++;
    pos++; // skip null
    return (pos + 3) & ~3; // pad to 4
}

// Read 16 bytes as uuid_hi (big-endian) and uuid_lo (big-endian)
static inline void read_uuid_be(const char* data, uint64_t* hi, uint64_t* lo) {
    uint64_t h = 0, l = 0;
    for (int i = 0; i < 8; i++) {
        h = (h << 8) | (uint8_t)data[i];
    }
    for (int i = 0; i < 8; i++) {
        l = (l << 8) | (uint8_t)data[8 + i];
    }
    *hi = h;
    *lo = l;
}

// Write int32 big-endian at position
static inline void write_int32_be(char* data, int32_t val) {
    data[0] = (char)((val >> 24) & 0xFF);
    data[1] = (char)((val >> 16) & 0xFF);
    data[2] = (char)((val >> 8) & 0xFF);
    data[3] = (char)(val & 0xFF);
}

// Read int32 big-endian from position
static inline int32_t read_int32_be(const char* data) {
    return ((int32_t)(uint8_t)data[0] << 24) |
           ((int32_t)(uint8_t)data[1] << 16) |
           ((int32_t)(uint8_t)data[2] << 8) |
           (int32_t)(uint8_t)data[3];
}

// Write UUID as 16 bytes big-endian
static inline void write_uuid_be(char* data, uint64_t hi, uint64_t lo) {
    for (int i = 7; i >= 0; i--) {
        data[7 - i] = (char)((hi >> (i * 8)) & 0xFF);
    }
    for (int i = 7; i >= 0; i--) {
        data[15 - i] = (char)((lo >> (i * 8)) & 0xFF);
    }
}

// Get the data size for an OSC type tag
static uint32_t osc_arg_size(char tag, const char* data, uint32_t pos, uint32_t size) {
    switch (tag) {
        case 'i': case 'f': return 4;
        case 'h': case 'd': case 't': return 8;
        case 'u': return 16;
        case 'T': case 'F': case 'N': case 'I': return 0;
        case 's': case 'S': {
            uint32_t end = osc_string_end(data, pos, size);
            return end - pos;
        }
        case 'b': {
            if (pos + 4 > size) return 0;
            uint32_t blob_len = ((uint8_t)data[pos] << 24) | ((uint8_t)data[pos+1] << 16) |
                                ((uint8_t)data[pos+2] << 8) | (uint8_t)data[pos+3];
            return 4 + ((blob_len + 3) & ~3);
        }
        default: return 0;
    }
}

// =============================================================================
// OUTBOUND REWRITING: u → i (in-place, messages shrink)
// =============================================================================

// Rewrite a single OSC message. Returns new size.
static uint32_t rewrite_message_uuid_to_int32(char* data, uint32_t size) {
    if (size < 4) return size;

    // Skip address string
    uint32_t pos = osc_string_end(data, 0, size);
    if (pos >= size || data[pos] != ',') return size;

    // Find type tag string
    uint32_t tags_start = pos + 1; // skip ','
    uint32_t tags_end = pos;
    while (tags_end < size && data[tags_end] != '\0') tags_end++;
    uint32_t num_tags = tags_end - tags_start;

    // Quick scan: any 'u' tags?
    bool has_uuid = false;
    for (uint32_t i = 0; i < num_tags; i++) {
        if (data[tags_start + i] == 'u') {
            has_uuid = true;
            break;
        }
    }
    if (!has_uuid) return size;

    // Skip past type tag string (with padding)
    uint32_t args_start = (tags_end + 1 + 3) & ~3;

    // Two-pointer rewrite
    uint32_t read_pos = args_start;
    uint32_t write_pos = args_start;

    for (uint32_t i = 0; i < num_tags; i++) {
        char tag = data[tags_start + i];

        if (tag == 'u') {
            // Read UUID, insert/lookup int32
            if (read_pos + 16 > size) break;
            uint64_t hi, lo;
            read_uuid_be(data + read_pos, &hi, &lo);
            int32_t node_id = uuid_insert(hi, lo);

            // Write int32 at write_pos
            write_int32_be(data + write_pos, node_id);

            // Change type tag from 'u' to 'i'
            data[tags_start + i] = 'i';

            write_pos += 4;
            read_pos += 16;
        } else {
            // Copy arg data as-is
            uint32_t arg_bytes = osc_arg_size(tag, data, read_pos, size);
            if (arg_bytes > 0 && write_pos != read_pos) {
                std::memmove(data + write_pos, data + read_pos, arg_bytes);
            }
            write_pos += arg_bytes;
            read_pos += arg_bytes;
        }
    }

    return write_pos;
}

// Check if data starts with "#bundle\0"
static inline bool is_osc_bundle(const char* data, uint32_t size) {
    return size >= 8 && data[0] == '#' && data[1] == 'b' && data[2] == 'u' &&
           data[3] == 'n' && data[4] == 'd' && data[5] == 'l' && data[6] == 'e' && data[7] == '\0';
}

// Forward declaration for recursive bundle rewriting
static uint32_t rewrite_bundle_uuid_to_int32(char* data, uint32_t size);

static uint32_t rewrite_packet_uuid_to_int32(char* data, uint32_t size) {
    if (is_osc_bundle(data, size)) {
        return rewrite_bundle_uuid_to_int32(data, size);
    } else {
        return rewrite_message_uuid_to_int32(data, size);
    }
}

static uint32_t rewrite_bundle_uuid_to_int32(char* data, uint32_t size) {
    if (size < 16) return size;

    // Skip "#bundle\0" (8 bytes) + timetag (8 bytes)
    uint32_t pos = 16;
    uint32_t write_pos = 16;

    while (pos + 4 <= size) {
        // Read sub-message size
        uint32_t msg_size = ((uint8_t)data[pos] << 24) | ((uint8_t)data[pos+1] << 16) |
                            ((uint8_t)data[pos+2] << 8) | (uint8_t)data[pos+3];
        pos += 4;

        if (pos + msg_size > size) break;

        // Move sub-message to write_pos + 4 if needed
        if (write_pos + 4 != pos) {
            std::memmove(data + write_pos + 4, data + pos, msg_size);
        }

        // Rewrite sub-message in place
        uint32_t new_msg_size = rewrite_packet_uuid_to_int32(data + write_pos + 4, msg_size);

        // Write new size prefix
        data[write_pos] = (char)((new_msg_size >> 24) & 0xFF);
        data[write_pos + 1] = (char)((new_msg_size >> 16) & 0xFF);
        data[write_pos + 2] = (char)((new_msg_size >> 8) & 0xFF);
        data[write_pos + 3] = (char)(new_msg_size & 0xFF);

        write_pos += 4 + new_msg_size;
        pos += msg_size;
    }

    return write_pos;
}

bool rewrite_uuid_to_int32(char* osc_data, uint32_t* payload_size) {
    if (!node_id_counter || *payload_size < 4) return false;

    uint32_t original_size = *payload_size;
    uint32_t new_size = rewrite_packet_uuid_to_int32(osc_data, original_size);

    if (new_size != original_size) {
        *payload_size = new_size;
        return true;
    }
    return false;
}

// =============================================================================
// INBOUND REWRITING: i → u (copy to static buffer, messages grow)
// =============================================================================

// Known node-lifecycle reply addresses where first arg is node ID
static bool is_node_lifecycle_address(const char* addr) {
    if (std::strcmp(addr, "/n_go") == 0) return true;
    if (std::strcmp(addr, "/n_end") == 0) return true;
    if (std::strcmp(addr, "/n_off") == 0) return true;
    if (std::strcmp(addr, "/n_on") == 0) return true;
    if (std::strcmp(addr, "/n_move") == 0) return true;
    if (std::strcmp(addr, "/n_info") == 0) return true;
    if (std::strcmp(addr, "/tr") == 0) return true;
    return false;
}

void rewrite_int32_to_uuid(const char* msg, int size, char* out_buf, int* out_size) {
    *out_size = size;  // Default: no change

    if (size < 8 || !node_id_counter) return;

    // Read address
    char addr[64];
    int addr_len = 0;
    while (addr_len < 63 && addr_len < size && msg[addr_len] != '\0') {
        addr[addr_len] = msg[addr_len];
        addr_len++;
    }
    addr[addr_len] = '\0';

    if (!is_node_lifecycle_address(addr)) return;

    // Find type tags
    uint32_t pos = osc_string_end(msg, 0, size);
    if (pos >= (uint32_t)size || msg[pos] != ',') return;

    uint32_t tags_start = pos + 1;
    uint32_t tags_end = pos;
    while (tags_end < (uint32_t)size && msg[tags_end] != '\0') tags_end++;
    uint32_t num_tags = tags_end - tags_start;

    if (num_tags == 0) return;

    // First arg must be 'i' for node ID
    if (msg[tags_start] != 'i') return;

    uint32_t args_start = (tags_end + 1 + 3) & ~3;
    if (args_start + 4 > (uint32_t)size) return;

    // Read first int32 arg
    int32_t node_id = read_int32_be(msg + args_start);

    // Look up UUID via reverse hash table (O(1))
    uint64_t uuid_hi, uuid_lo;
    if (!rev_find(node_id, &uuid_hi, &uuid_lo)) return;

    // Check output buffer size (message grows by 12 bytes per UUID)
    int new_size = size + 12;
    if (new_size > 4096) {
        worklet_debug("WARNING: reply rewrite overflow (%d > 4096), passing through as int32", new_size);
        return;
    }

    // Copy address + type tag string, modifying first tag from 'i' to 'u'
    std::memcpy(out_buf, msg, args_start);
    out_buf[tags_start] = 'u';

    // Write UUID (16 bytes) where int32 (4 bytes) was
    uint32_t out_pos = args_start;
    write_uuid_be(out_buf + out_pos, uuid_hi, uuid_lo);
    out_pos += 16;

    // Copy remaining args (skip first 4 bytes of original args)
    uint32_t remaining = size - (args_start + 4);
    if (remaining > 0) {
        std::memcpy(out_buf + out_pos, msg + args_start + 4, remaining);
        out_pos += remaining;
    }

    *out_size = out_pos;

    // On /n_end, remove the mapping (node freed)
    bool is_n_end = (addr[0] == '/' && addr[1] == 'n' && addr[2] == '_' &&
                     addr[3] == 'e' && addr[4] == 'n' && addr[5] == 'd' && addr[6] == '\0');
    if (is_n_end) {
        uuid_delete_by_id(node_id);
    }
}

// =============================================================================
// DIAGNOSTICS
// =============================================================================

uint32_t uuid_map_get_count() {
    return uuid_map_count;
}

uint32_t uuid_map_get_capacity() {
    return UUID_HASH_CAPACITY;
}

bool uuid_map_reverse_lookup(int32_t node_id, uint64_t* out_hi, uint64_t* out_lo) {
    return rev_find(node_id, out_hi, out_lo);
}

int32_t uuid_map_forward_lookup(uint64_t uuid_hi, uint64_t uuid_lo) {
    return fwd_find(uuid_hi, uuid_lo);
}
