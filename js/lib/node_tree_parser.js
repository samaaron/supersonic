// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
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
export const NODE_TREE_HEADER_SIZE   = 16;
export const NODE_TREE_ENTRY_SIZE    = 96;
export const NODE_TREE_DEF_NAME_SIZE = 32;

export function parseNodeTree(buffer, treeOffset, windowBytes) {

  // Read header (3 x uint32)
  const headerView = new Uint32Array(buffer, treeOffset, 3);
  // Version first: clockwork reads that word to decide when to copy the
  // window out, and reads nothing else in it.
  const version = headerView[0];
  const nodeCount = headerView[1];
  const droppedCount = headerView[2];

  // Read entries - each entry is 72 bytes: 6 int32s (24) + def_name (32) + uuid (16)
  // SuperSonic's own layout, not the host's. Clockwork reserves the window
  // and reads only its first word; the shape of everything after that is
  // ours, and these must match supersonic-node-mirror's NodeTreeHeader and
  // NodeEntry.
  const entriesBase = treeOffset + NODE_TREE_HEADER_SIZE;
  const maxNodes = Math.floor((windowBytes - NODE_TREE_HEADER_SIZE) / NODE_TREE_ENTRY_SIZE);
  const entrySize = NODE_TREE_ENTRY_SIZE;
  const defNameSize = NODE_TREE_DEF_NAME_SIZE;

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

    // Read UUID (two little-endian uint64s at offset 56 within entry).
    // Stored as big-endian-packed uint64 halves in WASM (little-endian),
    // so the bytes within each half are reversed.
    // Swap each 8-byte half back to big-endian (network) order.
    const uuidStart = entriesBase + byteOffset + 56;
    const uuidRaw = new Uint8Array(buffer, uuidStart, 16);
    let hasUuid = false;
    for (let j = 0; j < 16; j++) {
      if (uuidRaw[j] !== 0) { hasUuid = true; break; }
    }
    let uuid = null;
    if (hasUuid) {
      uuid = new Uint8Array(16);
      // Reverse bytes within each 8-byte half (LE uint64 → BE byte order)
      for (let j = 0; j < 8; j++) { uuid[j] = uuidRaw[7 - j]; }
      for (let j = 0; j < 8; j++) { uuid[8 + j] = uuidRaw[15 - j]; }
    }

    const node = {
      // INT32_MIN marks a real row with no compat alias — a clockwork-API
      // process the engine surface cannot address. -1 stays the
      // empty-slot sentinel, skipped above.
      id: id === -2147483648 ? null : id,
      parentId: dataView.getInt32(byteOffset + 4, true),
      isGroup: dataView.getInt32(byteOffset + 8, true) === 1,
      prevId: dataView.getInt32(byteOffset + 12, true),
      nextId: dataView.getInt32(byteOffset + 16, true),
      headId: dataView.getInt32(byteOffset + 20, true),
      defName,
      uuid
    };
    // Entry v2 — process-tree semantics, appended after the v1 fields.
    if (entrySize >= 96) {
      const parentUuidRaw = new Uint8Array(buffer, entriesBase + byteOffset + 72, 16);
      let hasParent = false;
      for (let j = 0; j < 16; j++) {
        if (parentUuidRaw[j] !== 0) { hasParent = true; break; }
      }
      let parentUuid = null;
      if (hasParent) {
        parentUuid = new Uint8Array(16);
        for (let j = 0; j < 8; j++) { parentUuid[j] = parentUuidRaw[7 - j]; }
        for (let j = 0; j < 8; j++) { parentUuid[8 + j] = parentUuidRaw[15 - j]; }
      }
      node.parentUuid = parentUuid;
      node.outPeak = dataView.getFloat32(byteOffset + 88, true);
      node.synthCount = dataView.getUint16(byteOffset + 92, true);
      node.listens = dataView.getUint8(byteOffset + 94) === 1;
    }
    nodes.push(node);
  }

  return { nodeCount, version, droppedCount, nodes };
}
