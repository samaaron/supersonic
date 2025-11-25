/*
    SuperSonic - SuperCollider AudioWorklet WebAssembly port
    Copyright (c) 2025 Sam Aaron

    Node Tree Management for SharedArrayBuffer

    SUPERSONIC-SPECIFIC FILE - Not part of upstream SuperCollider
    ============================================================
    This file is entirely new for Supersonic. It manages a flat array
    representation of the scsynth node tree in SharedArrayBuffer memory,
    enabling JavaScript to poll synth/group state at 60fps without OSC latency.

    Called from SC_Node.cpp (Node_StateMsg) on node lifecycle events.
*/

#include "node_tree.h"
#include "scsynth/server/SC_Group.h"  // For Node, Group structs
#include "scsynth/server/SC_SynthDef.h"  // For NodeDef (mName access)
#include <cstring>  // For strncpy

// Find index of node in tree (-1 if not found)
int32_t NodeTree_FindIndex(int32_t nodeId, NodeEntry* entries) {
    for (uint32_t i = 0; i < NODE_TREE_MAX_NODES; i++) {
        if (entries[i].id == nodeId) {
            return static_cast<int32_t>(i);
        }
    }
    return -1;
}

// Find first empty slot in tree (-1 if full)
int32_t NodeTree_FindEmptySlot(NodeEntry* entries) {
    for (uint32_t i = 0; i < NODE_TREE_MAX_NODES; i++) {
        if (entries[i].id == -1) {
            return static_cast<int32_t>(i);
        }
    }
    return -1;
}

// Add a node to the tree (called on kNode_Go)
void NodeTree_Add(Node* node, NodeTreeHeader* header, NodeEntry* entries) {
    if (!node || !header || !entries) return;

    int32_t slot = NodeTree_FindEmptySlot(entries);
    if (slot < 0) {
        // Tree is full, can't add more nodes
        return;
    }

    NodeEntry* entry = &entries[slot];
    entry->id = node->mID;
    entry->parent_id = node->mParent ? node->mParent->mNode.mID : -1;
    entry->is_group = node->mIsGroup ? 1 : 0;
    entry->prev_id = node->mPrev ? node->mPrev->mID : -1;
    entry->next_id = node->mNext ? node->mNext->mID : -1;

    if (node->mIsGroup) {
        Group* group = reinterpret_cast<Group*>(node);
        entry->head_id = group->mHead ? group->mHead->mID : -1;
        strncpy(entry->def_name, "group", NODE_TREE_DEF_NAME_SIZE - 1);
        entry->def_name[NODE_TREE_DEF_NAME_SIZE - 1] = '\0';
    } else {
        entry->head_id = -1;
        // Copy synthdef name from node->mDef->mName
        if (node->mDef) {
            strncpy(entry->def_name, (const char*)node->mDef->mName, NODE_TREE_DEF_NAME_SIZE - 1);
            entry->def_name[NODE_TREE_DEF_NAME_SIZE - 1] = '\0';
        } else {
            strncpy(entry->def_name, "unknown", NODE_TREE_DEF_NAME_SIZE - 1);
            entry->def_name[NODE_TREE_DEF_NAME_SIZE - 1] = '\0';
        }
    }

    // Update header
    uint32_t count = header->node_count.load(std::memory_order_relaxed);
    header->node_count.store(count + 1, std::memory_order_relaxed);
    header->version.fetch_add(1, std::memory_order_release);
}

// Remove a node from the tree (called on kNode_End)
void NodeTree_Remove(int32_t nodeId, NodeTreeHeader* header, NodeEntry* entries) {
    if (!header || !entries) return;

    int32_t slot = NodeTree_FindIndex(nodeId, entries);
    if (slot < 0) {
        // Node not found
        return;
    }

    // Mark slot as empty
    entries[slot].id = -1;

    // Update header
    uint32_t count = header->node_count.load(std::memory_order_relaxed);
    if (count > 0) {
        header->node_count.store(count - 1, std::memory_order_relaxed);
    }
    header->version.fetch_add(1, std::memory_order_release);
}

// Update a node's position in the tree (called on kNode_Move)
void NodeTree_Update(Node* node, NodeTreeHeader* header, NodeEntry* entries) {
    if (!node || !header || !entries) return;

    int32_t slot = NodeTree_FindIndex(node->mID, entries);
    if (slot < 0) {
        // Node not in tree - shouldn't happen, but add it
        NodeTree_Add(node, header, entries);
        return;
    }

    NodeEntry* entry = &entries[slot];
    entry->parent_id = node->mParent ? node->mParent->mNode.mID : -1;
    entry->prev_id = node->mPrev ? node->mPrev->mID : -1;
    entry->next_id = node->mNext ? node->mNext->mID : -1;

    if (node->mIsGroup) {
        Group* group = reinterpret_cast<Group*>(node);
        entry->head_id = group->mHead ? group->mHead->mID : -1;
    }

    // Bump version (position changed)
    header->version.fetch_add(1, std::memory_order_release);
}
