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
#include "audio_processor.h"  // For worklet_debug
#include "scsynth/server/SC_Group.h"  // For Node, Group structs
#include "scsynth/server/SC_SynthDef.h"  // For NodeDef (mName access)
#include <cstring>  // For strncpy

// Find index of node in tree (-1 if not found)
int32_t NodeTree_FindIndex(int32_t nodeId, NodeEntry* entries) {
    for (uint32_t i = 0; i < NODE_TREE_MIRROR_MAX_NODES; i++) {
        if (entries[i].id == nodeId) {
            return static_cast<int32_t>(i);
        }
    }
    return -1;
}

// Find first empty slot in tree (-1 if full)
int32_t NodeTree_FindEmptySlot(NodeEntry* entries) {
    for (uint32_t i = 0; i < NODE_TREE_MIRROR_MAX_NODES; i++) {
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
        // Mirror tree is full - actual scsynth tree continues working,
        // but JS won't see this node. Increment dropped_count so JS knows.
        uint32_t new_count = header->dropped_count.fetch_add(1, std::memory_order_relaxed) + 1;
        worklet_debug("[NodeTree] Mirror full! Node %d dropped, total dropped: %u", node->mID, new_count);
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

    // Update sibling nodes' prev/next pointers
    // If the new node has a previous sibling, update that sibling's next_id
    if (node->mPrev) {
        int32_t prevSlot = NodeTree_FindIndex(node->mPrev->mID, entries);
        if (prevSlot >= 0) {
            entries[prevSlot].next_id = node->mID;
        }
    }
    // If the new node has a next sibling, update that sibling's prev_id
    if (node->mNext) {
        int32_t nextSlot = NodeTree_FindIndex(node->mNext->mID, entries);
        if (nextSlot >= 0) {
            entries[nextSlot].prev_id = node->mID;
        }
    }

    // Update parent group's head_id if this node is now at the head
    if (node->mParent && !node->mPrev) {
        int32_t parentSlot = NodeTree_FindIndex(node->mParent->mNode.mID, entries);
        if (parentSlot >= 0) {
            entries[parentSlot].head_id = node->mID;
        }
    }

    // Update header
    uint32_t count = header->node_count.load(std::memory_order_relaxed);
    header->node_count.store(count + 1, std::memory_order_relaxed);
    header->version.fetch_add(1, std::memory_order_release);
}

// Remove a node from the mirror tree (called on kNode_End)
// IMPORTANT: This is only called from scsynth's Node_StateMsg callback when a real
// node is being destroyed. It is never called for non-existent node IDs - those are
// filtered out by meth_n_free before reaching Node_Delete/Node_Dtor/kNode_End.
void NodeTree_Remove(int32_t nodeId, NodeTreeHeader* header, NodeEntry* entries) {
    if (!header || !entries) return;

    int32_t slot = NodeTree_FindIndex(nodeId, entries);
    if (slot < 0) {
        // Node exists in scsynth but not in mirror - it was dropped due to overflow.
        // Since this callback only fires for real nodes, we can safely decrement.
        uint32_t dropped = header->dropped_count.load(std::memory_order_relaxed);
        if (dropped > 0) {
            header->dropped_count.fetch_sub(1, std::memory_order_relaxed);
        }
        return;
    }

    NodeEntry* entry = &entries[slot];

    // Update sibling nodes' prev/next pointers before removing
    // If this node had a previous sibling, update that sibling's next_id to point to our next
    if (entry->prev_id != -1) {
        int32_t prevSlot = NodeTree_FindIndex(entry->prev_id, entries);
        if (prevSlot >= 0) {
            entries[prevSlot].next_id = entry->next_id;
        }
    }
    // If this node had a next sibling, update that sibling's prev_id to point to our prev
    if (entry->next_id != -1) {
        int32_t nextSlot = NodeTree_FindIndex(entry->next_id, entries);
        if (nextSlot >= 0) {
            entries[nextSlot].prev_id = entry->prev_id;
        }
    }

    // Update parent group's head_id if this node was at the head
    if (entry->parent_id != -1 && entry->prev_id == -1) {
        int32_t parentSlot = NodeTree_FindIndex(entry->parent_id, entries);
        if (parentSlot >= 0) {
            // New head is our next sibling (or -1 if we were the only child)
            entries[parentSlot].head_id = entry->next_id;
        }
    }

    // Mark slot as empty
    entry->id = -1;

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

    // Get old sibling IDs before updating
    int32_t oldPrevId = entry->prev_id;
    int32_t oldNextId = entry->next_id;
    int32_t oldParentId = entry->parent_id;

    // Update this node's position
    entry->parent_id = node->mParent ? node->mParent->mNode.mID : -1;
    entry->prev_id = node->mPrev ? node->mPrev->mID : -1;
    entry->next_id = node->mNext ? node->mNext->mID : -1;

    if (node->mIsGroup) {
        Group* group = reinterpret_cast<Group*>(node);
        entry->head_id = group->mHead ? group->mHead->mID : -1;
    }

    // Update old siblings (patch the hole left by moving this node)
    if (oldPrevId != -1) {
        int32_t oldPrevSlot = NodeTree_FindIndex(oldPrevId, entries);
        if (oldPrevSlot >= 0) {
            entries[oldPrevSlot].next_id = oldNextId;
        }
    }
    if (oldNextId != -1) {
        int32_t oldNextSlot = NodeTree_FindIndex(oldNextId, entries);
        if (oldNextSlot >= 0) {
            entries[oldNextSlot].prev_id = oldPrevId;
        }
    }

    // Update old parent's head if this node was at the head
    if (oldParentId != -1 && oldPrevId == -1) {
        int32_t oldParentSlot = NodeTree_FindIndex(oldParentId, entries);
        if (oldParentSlot >= 0 && entries[oldParentSlot].head_id == node->mID) {
            entries[oldParentSlot].head_id = oldNextId;
        }
    }

    // Update new siblings
    if (node->mPrev) {
        int32_t newPrevSlot = NodeTree_FindIndex(node->mPrev->mID, entries);
        if (newPrevSlot >= 0) {
            entries[newPrevSlot].next_id = node->mID;
        }
    }
    if (node->mNext) {
        int32_t newNextSlot = NodeTree_FindIndex(node->mNext->mID, entries);
        if (newNextSlot >= 0) {
            entries[newNextSlot].prev_id = node->mID;
        }
    }

    // Update new parent's head_id if this node is now at the head
    if (node->mParent && !node->mPrev) {
        int32_t newParentSlot = NodeTree_FindIndex(node->mParent->mNode.mID, entries);
        if (newParentSlot >= 0) {
            entries[newParentSlot].head_id = node->mID;
        }
    }

    // Bump version (position changed)
    header->version.fetch_add(1, std::memory_order_release);
}
