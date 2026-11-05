// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2025-2026 Sam Aaron
/*
    node_tree.h — the node-tree mirror scsynth publishes in its window.

    SUPERSONIC-SPECIFIC FILE - Not part of upstream SuperCollider
    ============================================================
    A flat array of NodeEntry records, one per live node, that a client polls
    without asking the engine anything. The window clockwork hands the guest
    at dsp_new (DspConfig::shm_window) is laid out as a NodeTreeHeader
    followed by as many entries as fit:

    +---------------------+
    | NodeTreeHeader      |  16 bytes: version, node_count, dropped_count
    +---------------------+
    | NodeEntry[0]        |  NODE_TREE_ENTRY_SIZE bytes each
    | ...                 |
    | NodeEntry[N-1]      |  N = what the window holds (1024 at clockwork's
    +---------------------+      default window; raise the window to raise it)

    The array is sparse: a slot whose id is -1 is empty and reusable, and a
    removed node's slot is emptied rather than the array compacted, so slot
    indices stay stable. Siblings are a doubly linked chain (prev_id,
    next_id); a group names its first child (head_id). A node the window has
    no room for is counted in dropped_count and the engine's own tree carries
    on.

    Every write runs on the audio thread, from Node_StateMsg (SC_Node.cpp) on
    kNode_Go, kNode_End and kNode_Move, and from /n_order's reorder
    (SC_MiscCmds.cpp). None of them allocates: the free list of empty slots
    and the open-addressed id -> slot hash that keep each O(1) are sized once,
    in NodeTree_Bind, which dsp_new calls. The hash deletes by backward shift
    (Knuth's Algorithm R), never with tombstones, so a long session cannot
    fill it with graves.

    The header's words are atomics so a client on another thread reads a
    whole count; a reader that sees the version change re-reads. A client
    checks the window is this record, and which layout, from clockwork's
    arena table (NODE_TREE_WINDOW_MAGIC / _VERSION, declared by dsp_describe).
    The JavaScript reader is js/lib/node_tree_parser.js and carries the same
    two numbers; bump the version whenever NodeTreeHeader or NodeEntry change
    shape.
*/

#ifndef CLOCKWORK_NODE_TREE_H
#define CLOCKWORK_NODE_TREE_H

#include <atomic>
#include <cstdint>

constexpr uint32_t NODE_TREE_WINDOW_MAGIC   = 0x53434E54u;   // 'SCNT': scsynth node tree
constexpr uint32_t NODE_TREE_WINDOW_VERSION = 1u;
constexpr uint32_t NODE_TREE_HEADER_SIZE   = 16;
constexpr uint32_t NODE_TREE_DEF_NAME_SIZE = 32;
constexpr uint32_t NODE_TREE_ENTRY_SIZE    = 96;

struct alignas(4) NodeTreeHeader {
    std::atomic<uint32_t> version;       // bumped on every change; the host watches this
    std::atomic<uint32_t> node_count;    // entries in use
    std::atomic<uint32_t> dropped_count; // nodes the engine has that did not fit
    uint32_t _padding;
};

struct alignas(8) NodeEntry {
    int32_t id;         // -1 = empty slot; INT32_MIN = aliasless
    int32_t parent_id;
    int32_t is_group;   // 1 = group/process, 0 = leaf synth
    int32_t prev_id;
    int32_t next_id;
    int32_t head_id;
    char     def_name[NODE_TREE_DEF_NAME_SIZE];
    uint64_t uuid_hi;
    uint64_t uuid_lo;
    uint64_t parent_uuid_hi;
    uint64_t parent_uuid_lo;
    float    out_peak;
    uint16_t synth_count;
    uint8_t  listens;
    uint8_t  _pad;
};
static_assert(sizeof(NodeEntry) == NODE_TREE_ENTRY_SIZE,
              "NodeEntry must match the row stride the readers use");
static_assert(sizeof(NodeTreeHeader) == NODE_TREE_HEADER_SIZE,
              "NodeTreeHeader must match the header size the readers use");

// One instance's mirror: its window and the two indices over it. Held by the
// guest instance (scsynth_dsp.cpp), one per World; reached from the server
// through supersonic_node_tree.
struct NodeTreeHashEntry {
    int32_t key;    // node id, or NodeTreeMirror::kHashEmpty
    int32_t slot;   // index into the entries
};

struct NodeTreeMirror {
    static constexpr int32_t kHashEmpty = INT32_MIN;   // no node id is INT32_MIN

    uint8_t* window       = nullptr;
    uint32_t window_bytes = 0;
    uint32_t capacity     = 0;   // entries the window holds

    // Free list: free_next[i] is the next free slot after i (-1 = end). A
    // slot in use holds an undefined value: it is not on the list.
    int32_t* free_next = nullptr;
    int32_t  free_head = -1;

    // Open addressing with linear probing, sized to at most half full.
    NodeTreeHashEntry* hash      = nullptr;
    uint32_t           hash_mask = 0;

    bool bound() const { return window != nullptr; }
    NodeTreeHeader* header() const { return reinterpret_cast<NodeTreeHeader*>(window); }
    NodeEntry* entries() const { return reinterpret_cast<NodeEntry*>(window + NODE_TREE_HEADER_SIZE); }
};

struct World;
struct Node;

// The mirror of the World's instance while it is bound, else NULL: a World
// whose host reserved no window publishes nothing. Implemented in
// scsynth_dsp.cpp, where the instance is.
NodeTreeMirror* supersonic_node_tree(World* world);

// Bind the window: every entry reset to empty, the header zeroed, the free
// list and hash sized to what the window holds, in the instance's own memory
// through the World. Called from dsp_new, which is the one place the guest
// may allocate; a window too small for one entry leaves the mirror unbound.
void NodeTree_Bind(NodeTreeMirror* tree, World* world, void* window, uint32_t bytes);
// Let go of the window and the indices. Called from dsp_free, while the
// World is still there to free through.
void NodeTree_Unbind(NodeTreeMirror* tree, World* world);

// The lifecycle events, from Node_StateMsg: a node made (kNode_Go), freed
// (kNode_End) or moved (kNode_Move). Each fixes up the siblings and the
// parent's head and bumps the version.
void NodeTree_Add(NodeTreeMirror* tree, Node* node);
void NodeTree_Remove(NodeTreeMirror* tree, int32_t nodeId);
void NodeTree_Update(NodeTreeMirror* tree, Node* node);

#endif // CLOCKWORK_NODE_TREE_H
