// SPDX-License-Identifier: MIT OR GPL-3.0-or-later
// Copyright (c) 2025 Sam Aaron

/**
 * Parse node tree from buffer
 * Works with both SharedArrayBuffer (SAB mode) and regular ArrayBuffer (postMessage mode)
 *
 * @param {ArrayBuffer|SharedArrayBuffer} buffer - Buffer containing tree data
 * @param {number} treeOffset - Byte offset to tree data
 * @param {Object} bufferConstants - Layout constants
 * @returns {Object} {nodeCount, version, nodes}
 */
export function parseNodeTree(buffer, treeOffset, bufferConstants) {
  const bc = bufferConstants;

  // Read header (3 x uint32)
  const headerView = new Uint32Array(buffer, treeOffset, 3);
  const nodeCount = headerView[0];
  const version = headerView[1];
  const droppedCount = headerView[2];

  // Read entries - each entry is 56 bytes: 6 int32s (24 bytes) + def_name (32 bytes)
  const entriesBase = treeOffset + bc.NODE_TREE_HEADER_SIZE;
  const maxNodes = bc.NODE_TREE_MIRROR_MAX_NODES;
  const entrySize = bc.NODE_TREE_ENTRY_SIZE; // 56 bytes
  const defNameSize = bc.NODE_TREE_DEF_NAME_SIZE; // 32 bytes

  // Use DataView for mixed int32/string access
  const dataView = new DataView(buffer, entriesBase, maxNodes * entrySize);
  const textDecoder = new TextDecoder('utf-8');

  // Collect non-empty entries
  const nodes = [];
  let foundCount = 0;
  for (let i = 0; i < maxNodes && foundCount < nodeCount; i++) {
    const byteOffset = i * entrySize;
    const id = dataView.getInt32(byteOffset, true); // little-endian
    if (id === -1) continue; // Empty slot
    foundCount++;

    // Read def_name (32 bytes starting at byte 24 of entry)
    const defNameStart = entriesBase + byteOffset + 24;
    const defNameView = new Uint8Array(buffer, defNameStart, defNameSize);
    const defNameBytes = new Uint8Array(defNameSize);
    defNameBytes.set(defNameView); // Copy to non-shared buffer
    // Find null terminator
    let nullIndex = defNameBytes.indexOf(0);
    if (nullIndex === -1) nullIndex = defNameSize;
    const defName = textDecoder.decode(defNameBytes.subarray(0, nullIndex));

    nodes.push({
      id,
      parentId: dataView.getInt32(byteOffset + 4, true),
      isGroup: dataView.getInt32(byteOffset + 8, true) === 1,
      prevId: dataView.getInt32(byteOffset + 12, true),
      nextId: dataView.getInt32(byteOffset + 16, true),
      headId: dataView.getInt32(byteOffset + 20, true),
      defName
    });
  }

  return { nodeCount, version, droppedCount, nodes };
}
