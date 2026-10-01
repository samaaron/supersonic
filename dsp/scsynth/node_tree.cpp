// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2025-2026 Sam Aaron
/*
    node_tree.cpp — the node-tree mirror scsynth publishes in its window.

    SUPERSONIC-SPECIFIC FILE - Not part of upstream SuperCollider
    ============================================================
    A flat array of NodeEntry records, one per live node, that a client polls
    without asking the engine anything: the window clockwork hands the guest
    at dsp_new (DspConfig::shm_window) is laid out as NodeTreeHeader followed
    by as many entries as fit.

    Called from SC_Node.cpp (Node_StateMsg) on node lifecycle events, so every
    function here runs on the audio thread and none of them allocates: the
    free list and the id -> slot hash are sized once, in
    supersonic_node_tree_bind, which dsp_new calls.

    Two indices keep the audio-thread work O(1):

      - a free list of empty slots, so adding a node never scans the table;
      - an open-addressed hash from node id to slot, so finding one never does.

    The hash deletes by backward shift (Knuth's Algorithm R) rather than with
    tombstones, so a long session of creating and freeing nodes cannot fill it
    with graves and slow every lookup down.
*/

#include "node_tree.h"
#include "synth/server/SC_Group.h"     // For Node, Group structs
#include "synth/server/SC_SynthDef.h"  // For NodeDef (mName access)

#include <climits>
#include <cstring>

namespace {

uint8_t* g_window       = nullptr;
uint32_t g_window_bytes = 0;
uint32_t g_capacity     = 0;   // entries the window holds

// ── Free list: O(1) empty-slot allocation ───────────────────────────────────
// g_free_next[i] is the next free slot after i (-1 = end). Slots in use hold
// an undefined value: they are not on the list.
int32_t* g_free_next = nullptr;
int32_t  g_free_head = -1;

// ── Hash: O(1) node id -> slot ──────────────────────────────────────────────
// Open addressing with linear probing, sized to at most half full at capacity.
constexpr int32_t kHashEmpty = INT32_MIN;   // no node id is INT32_MIN
struct HashEntry {
    int32_t key;    // node id, or kHashEmpty
    int32_t slot;   // index into the entries
};
HashEntry* g_hash      = nullptr;
uint32_t   g_hash_mask = 0;

inline uint32_t hash_of(int32_t key) {
    // Murmur-style integer mix: node ids arrive as small consecutive numbers,
    // and this scatters them.
    uint32_t h = static_cast<uint32_t>(key);
    h ^= h >> 16;
    h *= 0x45d9f3bu;
    h ^= h >> 16;
    return h & g_hash_mask;
}

void hash_insert(int32_t key, int32_t slot) {
    uint32_t i = hash_of(key);
    while (g_hash[i].key != kHashEmpty) i = (i + 1) & g_hash_mask;
    g_hash[i].key  = key;
    g_hash[i].slot = slot;
}

int32_t hash_find(int32_t key) {
    if (!g_hash) return -1;
    uint32_t i = hash_of(key);
    while (g_hash[i].key != kHashEmpty) {
        if (g_hash[i].key == key) return g_hash[i].slot;
        i = (i + 1) & g_hash_mask;
    }
    return -1;
}

// Backward-shift deletion (Knuth Algorithm R): no tombstones.
void hash_remove(int32_t key) {
    uint32_t i = hash_of(key);
    while (g_hash[i].key != kHashEmpty) {
        if (g_hash[i].key == key) {
            for (;;) {
                g_hash[i].key = kHashEmpty;              // R1: empty the hole
                uint32_t j = i;
                for (;;) {
                    j = (j + 1) & g_hash_mask;           // R2: next occupant
                    if (g_hash[j].key == kHashEmpty) return;
                    const uint32_t r = hash_of(g_hash[j].key);   // R3: its natural slot
                    // R4: an entry whose natural slot lies cyclically in (i, j]
                    // is still reachable from it; leave it where it is.
                    if (i <= j) { if (i < r && r <= j) continue; }
                    else        { if (i < r || r <= j) continue; }
                    break;                                // it has to move up
                }
                g_hash[i] = g_hash[j];                    // R5: move it into the hole
                i = j;
            }
        }
        i = (i + 1) & g_hash_mask;
    }
}

uint32_t next_pow2(uint32_t v) {
    uint32_t p = 1;
    while (p < v) p <<= 1;
    return p;
}

void release_indices() {
    delete[] g_free_next; g_free_next = nullptr;
    delete[] g_hash;      g_hash = nullptr;
    g_hash_mask = 0;
    g_free_head = -1;
    g_capacity  = 0;
}

void set_def_name(NodeEntry* entry, const char* name) {
    std::strncpy(entry->def_name, name, NODE_TREE_DEF_NAME_SIZE - 1);
    entry->def_name[NODE_TREE_DEF_NAME_SIZE - 1] = '\0';
}

} // namespace

// ── The window ──────────────────────────────────────────────────────────────

void supersonic_node_tree_bind(void* window, uint32_t bytes) {
    release_indices();
    g_window       = static_cast<uint8_t*>(window);
    g_window_bytes = bytes;
    if (!g_window || bytes < NODE_TREE_HEADER_SIZE + NODE_TREE_ENTRY_SIZE) {
        g_window = nullptr;
        g_window_bytes = 0;
        return;
    }
    g_capacity = (bytes - NODE_TREE_HEADER_SIZE) / NODE_TREE_ENTRY_SIZE;
    // The host zeroes the window at every dsp_new and knows nothing of its
    // shape, so the empty marks (-1) and the header are ours to write.
    NodeTreeHeader* header = reinterpret_cast<NodeTreeHeader*>(g_window);
    header->version.store(0, std::memory_order_relaxed);
    header->node_count.store(0, std::memory_order_relaxed);
    header->dropped_count.store(0, std::memory_order_relaxed);
    NodeEntry* entries = reinterpret_cast<NodeEntry*>(g_window + NODE_TREE_HEADER_SIZE);
    for (uint32_t i = 0; i < g_capacity; ++i) {
        std::memset(&entries[i], 0, sizeof(NodeEntry));
        entries[i].id = -1;
    }
    // Sized here, in dsp_new, which is the one place the guest may allocate.
    g_free_next = new int32_t[g_capacity];
    const uint32_t buckets = next_pow2(g_capacity * 2);
    g_hash      = new HashEntry[buckets];
    g_hash_mask = buckets - 1;
    NodeTree_InitIndices();
}

void supersonic_node_tree_unbind() {
    release_indices();
    g_window = nullptr;
    g_window_bytes = 0;
}

uint32_t supersonic_node_tree_capacity() { return g_capacity; }

NodeTreeHeader* supersonic_node_tree_header() {
    return g_window ? reinterpret_cast<NodeTreeHeader*>(g_window) : nullptr;
}

NodeEntry* supersonic_node_tree_entries() {
    return g_window ? reinterpret_cast<NodeEntry*>(g_window + NODE_TREE_HEADER_SIZE) : nullptr;
}

// ── The indices ─────────────────────────────────────────────────────────────

// Rebuild the free list and clear the hash. Every entry is presumed empty:
// call this after the window's entries have been reset, not before.
void NodeTree_InitIndices() {
    if (!g_free_next || !g_hash) return;
    for (uint32_t i = 0; i + 1 < g_capacity; ++i) g_free_next[i] = static_cast<int32_t>(i + 1);
    g_free_next[g_capacity - 1] = -1;
    g_free_head = 0;
    for (uint32_t i = 0; i <= g_hash_mask; ++i) g_hash[i].key = kHashEmpty;
}

int32_t NodeTree_FindIndex(int32_t nodeId, NodeEntry* entries) {
    (void)entries;   // the lookup is the hash, not a scan
    return hash_find(nodeId);
}

int32_t NodeTree_FindEmptySlot(NodeEntry* entries) {
    (void)entries;   // allocation is the free list, not a scan
    return g_free_head;
}

// ── Lifecycle events ────────────────────────────────────────────────────────

void NodeTree_Add(Node* node, NodeTreeHeader* header, NodeEntry* entries) {
    if (!node || !header || !entries || !g_hash) return;

    const int32_t slot = NodeTree_FindEmptySlot(entries);
    if (slot < 0) {
        // The mirror is full. scsynth's own tree carries on; the client just
        // cannot see this node, and dropped_count says how many it is missing.
        header->dropped_count.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    g_free_head = g_free_next[slot];

    NodeEntry* entry = &entries[slot];
    std::memset(entry, 0, sizeof(*entry));
    entry->id        = node->mID;
    entry->parent_id = node->mParent ? node->mParent->mNode.mID : -1;
    entry->is_group  = node->mIsGroup ? 1 : 0;
    entry->prev_id   = node->mPrev ? node->mPrev->mID : -1;
    entry->next_id   = node->mNext ? node->mNext->mID : -1;
    if (node->mIsGroup) {
        Group* group = reinterpret_cast<Group*>(node);
        entry->head_id = group->mHead ? group->mHead->mID : -1;
        set_def_name(entry, "group");
    } else {
        entry->head_id = -1;
        set_def_name(entry, node->mDef ? (const char*)node->mDef->mName : "unknown");
    }

    // Into the hash before the sibling fix-ups, so a lookup of this node
    // during them finds it.
    hash_insert(node->mID, slot);

    if (node->mPrev) {
        const int32_t s = NodeTree_FindIndex(node->mPrev->mID, entries);
        if (s >= 0) entries[s].next_id = node->mID;
    }
    if (node->mNext) {
        const int32_t s = NodeTree_FindIndex(node->mNext->mID, entries);
        if (s >= 0) entries[s].prev_id = node->mID;
    }
    if (node->mParent && !node->mPrev) {
        const int32_t s = NodeTree_FindIndex(node->mParent->mNode.mID, entries);
        if (s >= 0) entries[s].head_id = node->mID;
    }

    header->node_count.fetch_add(1, std::memory_order_relaxed);
    header->version.fetch_add(1, std::memory_order_release);
}

// Only ever called for a node scsynth is really destroying: meth_n_free
// filters unknown ids before Node_Delete, so an id missing here was dropped
// on the way in, and the dropped count comes back down.
void NodeTree_Remove(int32_t nodeId, NodeTreeHeader* header, NodeEntry* entries) {
    if (!header || !entries || !g_hash) return;

    const int32_t slot = NodeTree_FindIndex(nodeId, entries);
    if (slot < 0) {
        uint32_t dropped = header->dropped_count.load(std::memory_order_relaxed);
        while (dropped > 0
               && !header->dropped_count.compare_exchange_weak(dropped, dropped - 1,
                                                               std::memory_order_relaxed)) {}
        return;
    }

    NodeEntry* entry = &entries[slot];
    if (entry->prev_id != -1) {
        const int32_t s = NodeTree_FindIndex(entry->prev_id, entries);
        if (s >= 0) entries[s].next_id = entry->next_id;
    }
    if (entry->next_id != -1) {
        const int32_t s = NodeTree_FindIndex(entry->next_id, entries);
        if (s >= 0) entries[s].prev_id = entry->prev_id;
    }
    if (entry->parent_id != -1 && entry->prev_id == -1) {
        const int32_t s = NodeTree_FindIndex(entry->parent_id, entries);
        if (s >= 0) entries[s].head_id = entry->next_id;   // the next sibling, or -1
    }

    hash_remove(nodeId);
    std::memset(entry, 0, sizeof(*entry));
    entry->id = -1;

    g_free_next[slot] = g_free_head;
    g_free_head = slot;

    uint32_t count = header->node_count.load(std::memory_order_relaxed);
    while (count > 0
           && !header->node_count.compare_exchange_weak(count, count - 1,
                                                        std::memory_order_relaxed)) {}
    header->version.fetch_add(1, std::memory_order_release);
}

void NodeTree_Update(Node* node, NodeTreeHeader* header, NodeEntry* entries) {
    if (!node || !header || !entries || !g_hash) return;

    const int32_t slot = NodeTree_FindIndex(node->mID, entries);
    if (slot < 0) {
        // Not mirrored — it was dropped when the mirror was full. A move is
        // as good a moment as any to try again.
        NodeTree_Add(node, header, entries);
        return;
    }

    NodeEntry* entry = &entries[slot];
    const int32_t oldPrevId   = entry->prev_id;
    const int32_t oldNextId   = entry->next_id;
    const int32_t oldParentId = entry->parent_id;

    entry->parent_id = node->mParent ? node->mParent->mNode.mID : -1;
    entry->prev_id   = node->mPrev ? node->mPrev->mID : -1;
    entry->next_id   = node->mNext ? node->mNext->mID : -1;
    if (node->mIsGroup) {
        Group* group = reinterpret_cast<Group*>(node);
        entry->head_id = group->mHead ? group->mHead->mID : -1;
    }

    // Close the gap where it was.
    if (oldPrevId != -1) {
        const int32_t s = NodeTree_FindIndex(oldPrevId, entries);
        if (s >= 0) entries[s].next_id = oldNextId;
    }
    if (oldNextId != -1) {
        const int32_t s = NodeTree_FindIndex(oldNextId, entries);
        if (s >= 0) entries[s].prev_id = oldPrevId;
    }
    if (oldParentId != -1 && oldPrevId == -1) {
        const int32_t s = NodeTree_FindIndex(oldParentId, entries);
        if (s >= 0 && entries[s].head_id == node->mID) entries[s].head_id = oldNextId;
    }

    // Splice it in where it is now.
    if (node->mPrev) {
        const int32_t s = NodeTree_FindIndex(node->mPrev->mID, entries);
        if (s >= 0) entries[s].next_id = node->mID;
    }
    if (node->mNext) {
        const int32_t s = NodeTree_FindIndex(node->mNext->mID, entries);
        if (s >= 0) entries[s].prev_id = node->mID;
    }
    if (node->mParent && !node->mPrev) {
        const int32_t s = NodeTree_FindIndex(node->mParent->mNode.mID, entries);
        if (s >= 0) entries[s].head_id = node->mID;
    }

    header->version.fetch_add(1, std::memory_order_release);
}
