// SPDX-License-Identifier: MIT OR GPL-3.0-or-later
// Copyright (c) 2025 Sam Aaron

import * as MetricsOffsets from './metrics_offsets.js';

/**
 * Inspect a SuperSonic instance or raw SharedArrayBuffer
 * Returns a snapshot of the current SAB state for debugging
 *
 * @param {Object} target - Instance or {sab, ringBufferBase, layout}
 * @returns {Object} Parsed SAB state
 *
 * @example
 * // From instance (via SuperSonic.inspect)
 * const info = inspect(sonic);
 *
 * // From raw components
 * const info = inspect({
 *   sab: sharedArrayBuffer,
 *   ringBufferBase: 16777216,
 *   layout: bufferConstants
 * });
 */
export function inspect(target) {
  let sab, ringBufferBase, layout;

  // Check if this is a SuperSonic-like instance
  if (target && typeof target.sharedBuffer !== 'undefined' && typeof target.ringBufferBase !== 'undefined') {
    sab = target.sharedBuffer;
    ringBufferBase = target.ringBufferBase;
    layout = target.bufferConstants;
  } else if (target && target.sab) {
    sab = target.sab;
    ringBufferBase = target.ringBufferBase ?? 0;
    layout = target.layout;
  } else {
    throw new Error('inspect() requires an instance or {sab, ringBufferBase, layout}');
  }

  if (!sab || !layout) {
    return { error: 'Not initialized - sab or layout missing' };
  }

  const atomicView = new Int32Array(sab);
  const controlBase = ringBufferBase + layout.CONTROL_START;

  // Read control pointers
  const control = {
    inHead: Atomics.load(atomicView, (controlBase + 0) / 4),
    inTail: Atomics.load(atomicView, (controlBase + 4) / 4),
    outHead: Atomics.load(atomicView, (controlBase + 8) / 4),
    outTail: Atomics.load(atomicView, (controlBase + 12) / 4),
    debugHead: Atomics.load(atomicView, (controlBase + 16) / 4),
    debugTail: Atomics.load(atomicView, (controlBase + 20) / 4),
    inSequence: Atomics.load(atomicView, (controlBase + 24) / 4),
    outSequence: Atomics.load(atomicView, (controlBase + 28) / 4),
    debugSequence: Atomics.load(atomicView, (controlBase + 32) / 4),
    statusFlags: Atomics.load(atomicView, (controlBase + 36) / 4),
    inWriteLock: Atomics.load(atomicView, (controlBase + 40) / 4),
  };

  // Read timing
  const ntpStartView = new Float64Array(sab, ringBufferBase + layout.NTP_START_TIME_START, 1);
  const driftView = new Int32Array(sab, ringBufferBase + layout.DRIFT_OFFSET_START, 1);
  const globalView = new Int32Array(sab, ringBufferBase + layout.GLOBAL_OFFSET_START, 1);

  const timing = {
    ntpStartTime: ntpStartView[0],
    driftOffsetMs: Atomics.load(driftView, 0),
    globalOffsetMs: Atomics.load(globalView, 0),
  };

  // Calculate buffer usage
  const inUsed = (control.inHead - control.inTail + layout.IN_BUFFER_SIZE) % layout.IN_BUFFER_SIZE;
  const outUsed = (control.outHead - control.outTail + layout.OUT_BUFFER_SIZE) % layout.OUT_BUFFER_SIZE;
  const debugUsed = (control.debugHead - control.debugTail + layout.DEBUG_BUFFER_SIZE) % layout.DEBUG_BUFFER_SIZE;

  const bufferUsage = {
    in: { bytes: inUsed, percent: (inUsed / layout.IN_BUFFER_SIZE) * 100 },
    out: { bytes: outUsed, percent: (outUsed / layout.OUT_BUFFER_SIZE) * 100 },
    debug: { bytes: debugUsed, percent: (debugUsed / layout.DEBUG_BUFFER_SIZE) * 100 },
  };

  // Read metrics
  const metricsView = new Uint32Array(sab, ringBufferBase + layout.METRICS_START, layout.METRICS_SIZE / 4);
  const metrics = {
    processCount: metricsView[MetricsOffsets.PROCESS_COUNT],
    messagesProcessed: metricsView[MetricsOffsets.MESSAGES_PROCESSED],
    messagesDropped: metricsView[MetricsOffsets.MESSAGES_DROPPED],
    schedulerQueueDepth: metricsView[MetricsOffsets.SCHEDULER_QUEUE_DEPTH],
    schedulerQueueMax: metricsView[MetricsOffsets.SCHEDULER_QUEUE_MAX],
    schedulerQueueDropped: metricsView[MetricsOffsets.SCHEDULER_QUEUE_DROPPED],
  };

  return {
    layout,
    ringBufferBase,
    control,
    timing,
    bufferUsage,
    metrics,
    sabByteLength: sab.byteLength,
  };
}

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

  // Read header (2 x uint32)
  const headerView = new Uint32Array(buffer, treeOffset, 2);
  const nodeCount = headerView[0];
  const version = headerView[1];

  // Read entries - each entry is 56 bytes: 6 int32s (24 bytes) + def_name (32 bytes)
  const entriesBase = treeOffset + bc.NODE_TREE_HEADER_SIZE;
  const maxNodes = bc.NODE_TREE_MAX_NODES;
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

  return { nodeCount, version, nodes };
}
