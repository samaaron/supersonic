/*
    SuperSonic - SuperCollider AudioWorklet WebAssembly port
    Copyright (c) 2025 Sam Aaron

    Node Tree Management for SharedArrayBuffer — the adapter.

    TAU-SPECIFIC FILE - Not part of upstream SuperCollider
    ============================================================
    The mirror itself is Rust (rust/supersonic-node-mirror): the slots, the
    sibling links, the free list and the id->slot hash. What stays here is the
    one thing Rust deliberately does not know — how to read a scsynth Node.

    That split is the point. The mirror is SuperSonic's idea, not the audio
    engine's, and both engines report into it; keeping the engine's structs out
    of it means the mirror has no opinion about which engine is running. This
    file translates, and the call sites in SC_Node.cpp are unchanged.
*/

#include "node_tree.h"

namespace {
uint8_t* g_window       = nullptr;
uint32_t g_window_bytes = 0;
}

void supersonic_node_tree_bind(void* window, uint32_t bytes) {
    g_window = static_cast<uint8_t*>(window);
    g_window_bytes = bytes;
}

NodeTreeHeader* supersonic_node_tree_header() {
    return (g_window && g_window_bytes >= NODE_TREE_HEADER_SIZE)
         ? reinterpret_cast<NodeTreeHeader*>(g_window) : nullptr;
}

NodeEntry* supersonic_node_tree_entries() {
    return (g_window && g_window_bytes >= NODE_TREE_HEADER_SIZE + NODE_TREE_ENTRY_SIZE)
         ? reinterpret_cast<NodeEntry*>(g_window + NODE_TREE_HEADER_SIZE) : nullptr;
}
#include "synth/server/SC_Group.h"     // For Node, Group structs
#include "synth/server/SC_SynthDef.h"  // For NodeDef (mName access)

// The Rust mirror. Declared here rather than in a generated header: six
// functions, and the struct below is the whole contract.
struct NodeFacts {
    int32_t     id;
    int32_t     parent_id;
    int         is_group;
    int32_t     prev_id;
    int32_t     next_id;
    int32_t     head_id;
    const char* def_name;
};

extern "C" {
void    supersonic_node_mirror_init(size_t capacity);
int32_t supersonic_node_mirror_find(int32_t nodeId);
int32_t supersonic_node_mirror_next_slot(void);
void    supersonic_node_mirror_add(const NodeFacts* facts, NodeTreeHeader* header, NodeEntry* entries);
void    supersonic_node_mirror_remove(int32_t nodeId, NodeTreeHeader* header, NodeEntry* entries);
void    supersonic_node_mirror_update(const NodeFacts* facts, NodeTreeHeader* header, NodeEntry* entries);
}

// Everything the mirror needs about a node, read once so the engine's structs
// go no further than this function.
static NodeFacts facts_of(Node* node) {
    NodeFacts f;
    f.id        = node->mID;
    f.parent_id = node->mParent ? node->mParent->mNode.mID : -1;
    f.is_group  = node->mIsGroup ? 1 : 0;
    f.prev_id   = node->mPrev ? node->mPrev->mID : -1;
    f.next_id   = node->mNext ? node->mNext->mID : -1;
    if (node->mIsGroup) {
        Group* group = reinterpret_cast<Group*>(node);
        f.head_id  = group->mHead ? group->mHead->mID : -1;
        f.def_name = nullptr;  // the mirror names every group "group"
    } else {
        f.head_id  = -1;
        f.def_name = node->mDef ? (const char*)node->mDef->mName : nullptr;
    }
    return f;
}

void NodeTree_InitIndices() {
    supersonic_node_mirror_init(NODE_TREE_MIRROR_MAX_NODES);
}

int32_t NodeTree_FindIndex(int32_t nodeId, NodeEntry* entries) {
    (void)entries;  // the lookup is the hash, not a scan
    return supersonic_node_mirror_find(nodeId);
}

int32_t NodeTree_FindEmptySlot(NodeEntry* entries) {
    (void)entries;  // allocation is the free list, not a scan
    return supersonic_node_mirror_next_slot();
}

void NodeTree_Add(Node* node, NodeTreeHeader* header, NodeEntry* entries) {
    if (!node || !header || !entries) return;
    NodeFacts f = facts_of(node);
    supersonic_node_mirror_add(&f, header, entries);
}

void NodeTree_Remove(int32_t nodeId, NodeTreeHeader* header, NodeEntry* entries) {
    supersonic_node_mirror_remove(nodeId, header, entries);
}

void NodeTree_Update(Node* node, NodeTreeHeader* header, NodeEntry* entries) {
    if (!node || !header || !entries) return;
    NodeFacts f = facts_of(node);
    supersonic_node_mirror_update(&f, header, entries);
}
