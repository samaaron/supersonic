/*
    SuperSonic - SuperCollider AudioWorklet WebAssembly port
    Copyright (c) 2025 Sam Aaron

    Node Tree Management for SharedArrayBuffer
    ==========================================

    TAU-SPECIFIC FILE - Not part of upstream SuperCollider
    ============================================================

    PURPOSE
    -------
    This module maintains a flat array representation of the scsynth node tree
    in SharedArrayBuffer memory. This enables JavaScript to poll the synth/group
    hierarchy at 60fps for visualization purposes without any OSC latency.

    In standard SuperCollider, inspecting the node tree requires sending OSC
    commands (/g_queryTree, /n_query) and waiting for replies. This round-trip
    latency makes smooth real-time visualization difficult. The SAB approach
    provides immediate, synchronous access to node state from JavaScript.


    MEMORY LAYOUT
    -------------
    The node tree lives in SharedArrayBuffer at NODE_TREE_START offset:

    +---------------------+
    | NodeTreeHeader      |  16 bytes
    | - node_count (4)    |  Number of active nodes in mirror
    | - version (4)       |  Change counter (for dirty checking)
    | - dropped_count (4) |  Nodes not mirrored due to overflow
    +---------------------+
    | NodeEntry[0]        |  NODE_TREE_ENTRY_SIZE bytes per entry
    | NodeEntry[1]        |
    | ...                 |
    | NodeEntry[N-1]      |  N = NODE_TREE_MIRROR_MAX_NODES, derived from the window
    +---------------------+

    Total size: ~73KB (16 + 1024 * 72 bytes)


    NODE ENTRY STRUCTURE (72 bytes)
    -------------------------------
    Each NodeEntry contains:
    - id (int32):        Node ID, or -1 for empty slots
    - parent_id (int32): Parent group ID, or -1 for root group
    - is_group (int32):  1 for groups, 0 for synths
    - prev_id (int32):   Previous sibling ID, or -1 if first child
    - next_id (int32):   Next sibling ID, or -1 if last child
    - head_id (int32):   First child ID (groups only), or -1
    - def_name (32B):    Synthdef name for synths, "group" for groups
    - uuid_hi (uint64):  Upper 8 bytes of UUID (0 if created with int32 ID)
    - uuid_lo (uint64):  Lower 8 bytes of UUID (0 if created with int32 ID)

    The array is sparse - slots with id == -1 are empty and reusable.
    When a node is removed, its slot is marked empty (id = -1) rather
    than compacting the array, which keeps slot indices stable.


    INTEGRATION WITH SCSYNTH
    ------------------------
    These functions are called from SC_Node.cpp (Node_StateMsg) whenever
    the node tree changes:

    - NodeTree_Add():    Called on kNode_Go (synth/group created)
    - NodeTree_Remove(): Called on kNode_End (synth/group freed)
    - NodeTree_Update(): Called on kNode_Move (node repositioned)

    Each operation updates the relevant entries and bumps the version
    counter, allowing JavaScript to detect changes efficiently:

        let lastVersion = 0;
        setInterval(() => {
            const tree = sonic.getTree();
            if (tree.version !== lastVersion) {
                lastVersion = tree.version;
                renderTree(tree.nodes);
            }
        }, 16);


    SIBLING CHAIN MAINTENANCE
    -------------------------
    The node tree maintains doubly-linked sibling chains via prev_id/next_id.
    When a node is added, removed, or moved, adjacent siblings are updated
    to maintain chain integrity. For example, removing node B from A->B->C
    requires updating A's next_id and C's prev_id to link A<->C directly.

    Groups also track their first child via head_id, which is updated when
    children are added/removed at the head position.


    THREAD SAFETY
    -------------
    The header uses std::atomic for node_count and version to ensure safe
    concurrent access between the audio thread (WASM) writing updates and
    the main thread (JavaScript) reading state. Individual NodeEntry fields
    are not atomic, but the version counter ensures readers can detect
    mid-update states and re-read if needed.


    LIMITATIONS
    -----------
    - NODE_TREE_MIRROR_MAX_NODES nodes in the mirror: what the window holds
      at NODE_TREE_ENTRY_SIZE bytes each, after the header (1024 at the default
      CLOCKWORK_WINDOW_BYTES). Raise the window to raise it.
    - If actual scsynth tree exceeds this, excess nodes are not mirrored
      (dropped_count tracks how many; audio continues working)
    - Synthdef names truncated to 31 characters
    - No control/parameter values exposed (use OSC for that)

    See node_tree.cpp for implementation details.
    See test/node_tree.spec.mjs for comprehensive behavioral tests.
*/

#ifndef CLOCKWORK_NODE_TREE_H
#define CLOCKWORK_NODE_TREE_H

#include "shared_memory.h"   // the window's extent, not its shape

#include <atomic>
#include <cstdint>

// ── The record SuperSonic publishes ─────────────────────────────────────────
//
// Ours, not the host's. Clockwork reserves DspConfig::shm_window, reads its
// FIRST uint32 to know when the window is worth copying out, and reads
// nothing else — so everything below is between this writer and the clients
// that parse it (js/lib/node_tree_parser.js, and the same layout in
// rust/supersonic-node-mirror).
//
// VERSION IS FIRST because that word is the host's one convention. The rest
// is ours to order.
constexpr uint32_t NODE_TREE_HEADER_SIZE   = 16;
constexpr uint32_t NODE_TREE_DEF_NAME_SIZE = 32;
constexpr uint32_t NODE_TREE_ENTRY_SIZE    = 96;
constexpr uint32_t NODE_TREE_MIRROR_MAX_NODES =
    (SHM_WINDOW_SIZE - NODE_TREE_HEADER_SIZE) / NODE_TREE_ENTRY_SIZE;

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

// Where our tree lives: the window clockwork handed us in
// DspConfig::shm_window, published once at dsp_new. NULL before the guest is
// built, or when the host reserved no window.
//
// One accessor rather than three copies of `shared_memory + NODE_TREE_START`:
// that offset is the HOST's layout, and reaching for it meant the guest had to
// know where clockwork keeps things.
NodeTreeHeader* supersonic_node_tree_header();
NodeEntry*      supersonic_node_tree_entries();
void            supersonic_node_tree_bind(void* window, uint32_t bytes);

// Forward declarations
struct Node;
struct Group;

// =============================================================================
// NODE TREE MANAGEMENT FUNCTIONS
// =============================================================================
// These update the SharedArrayBuffer node tree for JS polling.
// Called from SC_Node.cpp (Node_StateMsg) on node lifecycle events.

/**
 * Add a node to the tree.
 * Called on kNode_Go when a synth or group is created.
 *
 * Updates:
 * - Finds an empty slot and populates it with node data
 * - Updates prev sibling's next_id to point to new node
 * - Updates next sibling's prev_id to point to new node
 * - Updates parent group's head_id if new node is at head
 * - Increments node_count and version
 *
 * @param node    The scsynth Node that was created
 * @param header  Pointer to NodeTreeHeader in SharedArrayBuffer
 * @param entries Pointer to NodeEntry array in SharedArrayBuffer
 */
void NodeTree_Add(Node* node, NodeTreeHeader* header, NodeEntry* entries);

/**
 * Remove a node from the tree.
 * Called on kNode_End when a synth or group is freed.
 *
 * Updates:
 * - Patches sibling chain (prev's next -> our next, next's prev -> our prev)
 * - Updates parent group's head_id if removed node was at head
 * - Marks slot as empty (id = -1)
 * - Decrements node_count and increments version
 *
 * @param nodeId  The ID of the node being removed
 * @param header  Pointer to NodeTreeHeader in SharedArrayBuffer
 * @param entries Pointer to NodeEntry array in SharedArrayBuffer
 */
void NodeTree_Remove(int32_t nodeId, NodeTreeHeader* header, NodeEntry* entries);

/**
 * Update a node's position in the tree.
 * Called on kNode_Move when a node is repositioned (n_before, n_after, g_head, g_tail).
 *
 * Updates:
 * - Patches old sibling chain to close the gap
 * - Updates old parent's head_id if node was at head
 * - Updates this node's parent_id, prev_id, next_id
 * - Patches new sibling chain to include this node
 * - Updates new parent's head_id if node is now at head
 * - Increments version
 *
 * @param node    The scsynth Node that was moved
 * @param header  Pointer to NodeTreeHeader in SharedArrayBuffer
 * @param entries Pointer to NodeEntry array in SharedArrayBuffer
 */
void NodeTree_Update(Node* node, NodeTreeHeader* header, NodeEntry* entries);

/**
 * Initialize free list and hash table indices.
 * Must be called once after node tree memory is zeroed (all entries id == -1).
 * Called from init_memory() in audio_processor.cpp.
 */
void NodeTree_InitIndices();

/**
 * Find the array index of a node by ID.
 *
 * @param nodeId  The node ID to search for
 * @param entries Pointer to NodeEntry array in SharedArrayBuffer
 * @return        Array index (0 to NODE_TREE_MIRROR_MAX_NODES-1), or -1 if not found
 */
int32_t NodeTree_FindIndex(int32_t nodeId, NodeEntry* entries);

/**
 * Find the first empty slot in the tree.
 *
 * @param entries Pointer to NodeEntry array in SharedArrayBuffer
 * @return        Array index of first empty slot (id == -1), or -1 if full
 */
int32_t NodeTree_FindEmptySlot(NodeEntry* entries);

#endif // CLOCKWORK_NODE_TREE_H
