// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2025-2026 Sam Aaron
// node_tree.cpp — the node-tree mirror; see node_tree.h.

#include "node_tree.h"
#include "guest_heap.h"
#include "SC_Group.h"     // Node, Group
#include "SC_SynthDef.h"  // NodeDef (mName)

#include <climits>
#include <cstring>

namespace {

inline uint32_t hash_of(const NodeTreeMirror* t, int32_t key) {
    // Murmur-style integer mix: node ids arrive as small consecutive numbers,
    // and this scatters them.
    uint32_t h = static_cast<uint32_t>(key);
    h ^= h >> 16;
    h *= 0x45d9f3bu;
    h ^= h >> 16;
    return h & t->hash_mask;
}

void hash_insert(NodeTreeMirror* t, int32_t key, int32_t slot) {
    uint32_t i = hash_of(t, key);
    while (t->hash[i].key != NodeTreeMirror::kHashEmpty) i = (i + 1) & t->hash_mask;
    t->hash[i].key  = key;
    t->hash[i].slot = slot;
}

int32_t hash_find(const NodeTreeMirror* t, int32_t key) {
    uint32_t i = hash_of(t, key);
    while (t->hash[i].key != NodeTreeMirror::kHashEmpty) {
        if (t->hash[i].key == key) return t->hash[i].slot;
        i = (i + 1) & t->hash_mask;
    }
    return -1;
}

// Backward-shift deletion (Knuth Algorithm R): no tombstones.
void hash_remove(NodeTreeMirror* t, int32_t key) {
    NodeTreeHashEntry* hash = t->hash;
    const uint32_t mask = t->hash_mask;
    uint32_t i = hash_of(t, key);
    while (hash[i].key != NodeTreeMirror::kHashEmpty) {
        if (hash[i].key == key) {
            for (;;) {
                hash[i].key = NodeTreeMirror::kHashEmpty;   // R1: empty the hole
                uint32_t j = i;
                for (;;) {
                    j = (j + 1) & mask;                      // R2: next occupant
                    if (hash[j].key == NodeTreeMirror::kHashEmpty) return;
                    const uint32_t r = hash_of(t, hash[j].key);   // R3: its natural slot
                    // R4: an entry whose natural slot lies cyclically in (i, j]
                    // is still reachable from it; leave it where it is.
                    if (i <= j) { if (i < r && r <= j) continue; }
                    else        { if (i < r || r <= j) continue; }
                    break;                                   // it has to move up
                }
                hash[i] = hash[j];                           // R5: move it into the hole
                i = j;
            }
        }
        i = (i + 1) & mask;
    }
}

uint32_t next_pow2(uint32_t v) {
    uint32_t p = 1;
    while (p < v) p <<= 1;
    return p;
}

void set_def_name(NodeEntry* entry, const char* name) {
    std::strncpy(entry->def_name, name, NODE_TREE_DEF_NAME_SIZE - 1);
    entry->def_name[NODE_TREE_DEF_NAME_SIZE - 1] = '\0';
}

// The slot a node is mirrored in, or -1.
inline int32_t slot_of(const NodeTreeMirror* t, int32_t nodeId) { return hash_find(t, nodeId); }

} // namespace

// ── The window ──────────────────────────────────────────────────────────────

void NodeTree_Bind(NodeTreeMirror* t, World* world, void* window, uint32_t bytes) {
    *t = NodeTreeMirror{};
    if (!window || bytes < NODE_TREE_HEADER_SIZE + NODE_TREE_ENTRY_SIZE) return;
    t->window       = static_cast<uint8_t*>(window);
    t->window_bytes = bytes;
    t->capacity     = (bytes - NODE_TREE_HEADER_SIZE) / NODE_TREE_ENTRY_SIZE;
    // The host zeroes the window at every dsp_new and knows nothing of its
    // shape, so the empty marks (-1) and the header are ours to write.
    NodeTreeHeader* header = t->header();
    header->version.store(0, std::memory_order_relaxed);
    header->node_count.store(0, std::memory_order_relaxed);
    header->dropped_count.store(0, std::memory_order_relaxed);
    NodeEntry* entries = t->entries();
    for (uint32_t i = 0; i < t->capacity; ++i) {
        std::memset(&entries[i], 0, sizeof(NodeEntry));
        entries[i].id = -1;
    }
    // Sized here, in dsp_new, which is the one place the guest may allocate,
    // and in the instance's own memory (guest_heap.h).
    t->free_next = guest_new_array<int32_t>(world, t->capacity);
    const uint32_t buckets = next_pow2(t->capacity * 2);
    t->hash      = guest_new_array<NodeTreeHashEntry>(world, buckets);
    t->hash_mask = buckets - 1;
    for (uint32_t i = 0; i + 1 < t->capacity; ++i) t->free_next[i] = static_cast<int32_t>(i + 1);
    t->free_next[t->capacity - 1] = -1;
    t->free_head = 0;
    for (uint32_t i = 0; i <= t->hash_mask; ++i) t->hash[i].key = NodeTreeMirror::kHashEmpty;
}

void NodeTree_Unbind(NodeTreeMirror* t, World* world) {
    guest_delete_array(world, t->free_next);
    guest_delete_array(world, t->hash);
    *t = NodeTreeMirror{};
}

// ── Lifecycle events ────────────────────────────────────────────────────────

void NodeTree_Add(NodeTreeMirror* t, Node* node) {
    if (!node || !t || !t->bound()) return;
    NodeTreeHeader* header  = t->header();
    NodeEntry*      entries = t->entries();

    const int32_t slot = t->free_head;
    if (slot < 0) {
        // The mirror is full. scsynth's own tree carries on; the client just
        // cannot see this node, and dropped_count says how many it is missing.
        header->dropped_count.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    t->free_head = t->free_next[slot];

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
    hash_insert(t, node->mID, slot);

    if (node->mPrev) {
        const int32_t s = slot_of(t, node->mPrev->mID);
        if (s >= 0) entries[s].next_id = node->mID;
    }
    if (node->mNext) {
        const int32_t s = slot_of(t, node->mNext->mID);
        if (s >= 0) entries[s].prev_id = node->mID;
    }
    if (node->mParent && !node->mPrev) {
        const int32_t s = slot_of(t, node->mParent->mNode.mID);
        if (s >= 0) entries[s].head_id = node->mID;
    }

    header->node_count.fetch_add(1, std::memory_order_relaxed);
    header->version.fetch_add(1, std::memory_order_release);
}

// Only ever called for a node scsynth is really destroying: meth_n_free
// filters unknown ids before Node_Delete, so an id missing here was dropped
// on the way in, and the dropped count comes back down.
void NodeTree_Remove(NodeTreeMirror* t, int32_t nodeId) {
    if (!t || !t->bound()) return;
    NodeTreeHeader* header  = t->header();
    NodeEntry*      entries = t->entries();

    const int32_t slot = slot_of(t, nodeId);
    if (slot < 0) {
        uint32_t dropped = header->dropped_count.load(std::memory_order_relaxed);
        while (dropped > 0
               && !header->dropped_count.compare_exchange_weak(dropped, dropped - 1,
                                                               std::memory_order_relaxed)) {}
        return;
    }

    NodeEntry* entry = &entries[slot];
    if (entry->prev_id != -1) {
        const int32_t s = slot_of(t, entry->prev_id);
        if (s >= 0) entries[s].next_id = entry->next_id;
    }
    if (entry->next_id != -1) {
        const int32_t s = slot_of(t, entry->next_id);
        if (s >= 0) entries[s].prev_id = entry->prev_id;
    }
    if (entry->parent_id != -1 && entry->prev_id == -1) {
        const int32_t s = slot_of(t, entry->parent_id);
        if (s >= 0) entries[s].head_id = entry->next_id;   // the next sibling, or -1
    }

    hash_remove(t, nodeId);
    std::memset(entry, 0, sizeof(*entry));
    entry->id = -1;

    t->free_next[slot] = t->free_head;
    t->free_head = slot;

    uint32_t count = header->node_count.load(std::memory_order_relaxed);
    while (count > 0
           && !header->node_count.compare_exchange_weak(count, count - 1,
                                                        std::memory_order_relaxed)) {}
    header->version.fetch_add(1, std::memory_order_release);
}

void NodeTree_Update(NodeTreeMirror* t, Node* node) {
    if (!node || !t || !t->bound()) return;
    NodeTreeHeader* header  = t->header();
    NodeEntry*      entries = t->entries();

    const int32_t slot = slot_of(t, node->mID);
    if (slot < 0) {
        // Not mirrored — it was dropped when the mirror was full. A move is
        // as good a moment as any to try again.
        NodeTree_Add(t, node);
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
        const int32_t s = slot_of(t, oldPrevId);
        if (s >= 0) entries[s].next_id = oldNextId;
    }
    if (oldNextId != -1) {
        const int32_t s = slot_of(t, oldNextId);
        if (s >= 0) entries[s].prev_id = oldPrevId;
    }
    if (oldParentId != -1 && oldPrevId == -1) {
        const int32_t s = slot_of(t, oldParentId);
        if (s >= 0 && entries[s].head_id == node->mID) entries[s].head_id = oldNextId;
    }

    // Splice it in where it is now.
    if (node->mPrev) {
        const int32_t s = slot_of(t, node->mPrev->mID);
        if (s >= 0) entries[s].next_id = node->mID;
    }
    if (node->mNext) {
        const int32_t s = slot_of(t, node->mNext->mID);
        if (s >= 0) entries[s].prev_id = node->mID;
    }
    if (node->mParent && !node->mPrev) {
        const int32_t s = slot_of(t, node->mParent->mNode.mID);
        if (s >= 0) entries[s].head_id = node->mID;
    }

    header->version.fetch_add(1, std::memory_order_release);
}
