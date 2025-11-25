/*
    SuperSonic - SuperCollider AudioWorklet WebAssembly port
    Copyright (c) 2025 Sam Aaron

    Node Tree Management for SharedArrayBuffer
    Maintains a flat array of nodes for efficient polling by JavaScript visualizations

    SUPERSONIC-SPECIFIC FILE - Not part of upstream SuperCollider
    ============================================================
    This header declares functions for managing the node tree in SharedArrayBuffer.
    See node_tree.cpp for implementation details.
*/

#ifndef SUPERSONIC_NODE_TREE_H
#define SUPERSONIC_NODE_TREE_H

#include "shared_memory.h"

// Forward declarations
struct Node;
struct Group;

// Node tree management functions
// These update the SharedArrayBuffer node tree for JS polling

// Add a node to the tree (called on kNode_Go)
void NodeTree_Add(Node* node, NodeTreeHeader* header, NodeEntry* entries);

// Remove a node from the tree (called on kNode_End)
void NodeTree_Remove(int32_t nodeId, NodeTreeHeader* header, NodeEntry* entries);

// Update a node's position in the tree (called on kNode_Move)
void NodeTree_Update(Node* node, NodeTreeHeader* header, NodeEntry* entries);

// Find index of node in tree (-1 if not found)
int32_t NodeTree_FindIndex(int32_t nodeId, NodeEntry* entries);

// Find first empty slot in tree (-1 if full)
int32_t NodeTree_FindEmptySlot(NodeEntry* entries);

#endif // SUPERSONIC_NODE_TREE_H
