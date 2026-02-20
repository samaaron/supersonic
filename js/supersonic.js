// SPDX-License-Identifier: MIT OR GPL-3.0-or-later
// Copyright (c) 2025 Sam Aaron

/**
 * SuperSonic - WebAssembly SuperCollider synthesis engine
 * Coordinates SharedArrayBuffer, WASM, AudioWorklet, and IO Workers
 */

import { createTransport, OscChannel } from "./lib/transport/index.js";
import { shouldBypass, readTimetag, getCurrentNTPFromPerformance } from "./lib/osc_classifier.js";

// Re-export OscChannel for use in workers
export { OscChannel };
// Re-export osc utilities for direct use in workers (zero-allocation OSC encoding)
export { oscFast as osc };
import { BufferManager } from "./lib/buffer_manager.js";
import { AssetLoader } from "./lib/asset_loader.js";
import { OSCRewriter } from "./lib/osc_rewriter.js";
import { extractSynthDefName } from "./lib/synthdef_parser.js";
import { EventEmitter } from "./lib/event_emitter.js";
import { MetricsReader } from "./lib/metrics_reader.js";
import { NTPTiming } from "./lib/ntp_timing.js";
import { AudioCapture } from "./lib/audio_capture.js";
import { parseNodeTree } from "./lib/node_tree_parser.js";
import * as oscFast from "./lib/osc_fast.js";
import { SYNC_TIMEOUT_MS, WORKLET_INIT_TIMEOUT_MS, SNAPSHOT_INTERVAL_MS } from "./timing_constants.js";
import { MemoryLayout } from "./memory_layout.js";
import { defaultWorldOptions } from "./scsynth_options.js";
import { addWorkletModule } from "./lib/worker_loader.js";

/**
 * @typedef {import('./lib/metrics_types.js').SuperSonicMetrics} SuperSonicMetrics
 */

export class SuperSonic {
  // Expose OSC utilities as static methods (uses plain args, not typed {type, value} format)
  static osc = {
    encodeMessage: (address, args) => oscFast.copyEncoded(oscFast.encodeMessage(address, args)),
    encodeBundle: (timeTag, packets) => oscFast.copyEncoded(oscFast.encodeBundle(timeTag, packets)),
    decode: (data) => oscFast.decodePacket(data),
    encodeSingleBundle: (timeTag, address, args) =>
      oscFast.copyEncoded(oscFast.encodeSingleBundle(timeTag, address, args)),
    readTimetag: (bundleData) => readTimetag(bundleData),
    ntpNow: () => getCurrentNTPFromPerformance(),
    NTP_EPOCH_OFFSET: oscFast.NTP_EPOCH_OFFSET,
    // Backwards-compatible encode for tests - handles legacy osc.js format
    // Deprecated: use encodeMessage() or encodeBundle() instead
    encode: (packet) => {
      if (!SuperSonic._encodeDeprecationWarned) {
        console.warn('SuperSonic.osc.encode() is deprecated. Use encodeMessage() or encodeBundle().');
        SuperSonic._encodeDeprecationWarned = true;
      }
      if (packet.timeTag !== undefined) {
        // Bundle - convert legacy format
        let timeTag;
        if (packet.timeTag.raw) {
          // Convert { raw: [seconds, fraction] } to NTP float
          const [seconds, fraction] = packet.timeTag.raw;
          timeTag = seconds + fraction / oscFast.TWO_POW_32;
        } else if (typeof packet.timeTag === 'number') {
          timeTag = packet.timeTag;
        } else {
          timeTag = 1; // immediate
        }
        // Convert typed args to plain args as arrays
        const packets = packet.packets.map(p => {
          const args = (p.args || []).map(a =>
            (a && typeof a === 'object' && 'value' in a) ? a.value : a
          );
          return [p.address, ...args];
        });
        return oscFast.copyEncoded(oscFast.encodeBundle(timeTag, packets));
      } else {
        // Message - convert typed args to plain args
        const args = (packet.args || []).map(a =>
          (a && typeof a === 'object' && 'value' in a) ? a.value : a
        );
        return oscFast.copyEncoded(oscFast.encodeMessage(packet.address, args));
      }
    },
  };

  /**
   * Get schema describing all available metrics with array offsets and UI layout.
   *
   * - `metrics`: each key maps to { offset, type, unit, description } for the merged Uint32Array
   * - `layout`: panel structure for rendering a metrics UI
   * - `sentinels`: magic values used in the metrics array
   *
   * The `getMetrics()` object API is unchanged — this schema adds array-offset
   * metadata and a declarative layout on top.
   */
  static #metricsSchema = null;
  static getMetricsSchema() {
    return (this.#metricsSchema ??= {
      metrics: {
        // scsynth metrics [0-8]
        scsynthProcessCount:          { offset: 0,  type: 'counter',  unit: 'count', description: 'Audio process() calls' },
        scsynthMessagesProcessed:     { offset: 1,  type: 'counter',  unit: 'count', description: 'OSC messages processed by scsynth' },
        scsynthMessagesDropped:       { offset: 2,  type: 'counter',  unit: 'count', description: 'Messages dropped (ring buffer full)' },
        scsynthSchedulerDepth:        { offset: 3,  type: 'gauge',    unit: 'count', description: 'Current scheduler queue depth' },
        scsynthSchedulerPeakDepth:    { offset: 4,  type: 'gauge',    unit: 'count', description: 'Peak scheduler queue depth (high water mark)' },
        scsynthSchedulerDropped:      { offset: 5,  type: 'counter',  unit: 'count', description: 'Scheduled events dropped' },
        scsynthSequenceGaps:          { offset: 6,  type: 'counter',  unit: 'count', description: 'Messages lost in transit from JS to scsynth' },
        scsynthWasmErrors:            { offset: 7,  type: 'counter',  unit: 'count', description: 'WASM execution errors in audio worklet' },
        scsynthSchedulerLates:        { offset: 8,  type: 'counter',  unit: 'count', description: 'Bundles executed after their scheduled time' },

        // Prescheduler metrics [9-23]
        preschedulerPending:          { offset: 9,  type: 'gauge',    unit: 'count', description: 'Events waiting to be scheduled' },
        preschedulerPendingPeak:      { offset: 10, type: 'gauge',    unit: 'count', description: 'Peak pending events' },
        preschedulerBundlesScheduled: { offset: 11, type: 'counter',  unit: 'count', description: 'Bundles scheduled' },
        preschedulerDispatched:       { offset: 12, type: 'counter',  unit: 'count', description: 'Events sent to worklet' },
        preschedulerEventsCancelled:  { offset: 13, type: 'counter',  unit: 'count', description: 'Events cancelled' },
        preschedulerMinHeadroomMs:    { offset: 14, type: 'gauge',    unit: 'ms',    description: 'Smallest time gap between JS prescheduler dispatch and scsynth scheduler execution' },
        preschedulerLates:            { offset: 15, type: 'counter',  unit: 'count', description: 'Bundles dispatched after their scheduled execution time' },
        preschedulerRetriesSucceeded: { offset: 16, type: 'counter',  unit: 'count', description: 'Retries that succeeded' },
        preschedulerRetriesFailed:    { offset: 17, type: 'counter',  unit: 'count', description: 'Retries that failed' },
        preschedulerRetryQueueSize:   { offset: 18, type: 'gauge',    unit: 'count', description: 'Current retry queue size' },
        preschedulerRetryQueuePeak:   { offset: 19, type: 'gauge',    unit: 'count', description: 'Peak retry queue size' },
        preschedulerMessagesRetried:  { offset: 20, type: 'counter',  unit: 'count', description: 'Messages that needed retry' },
        preschedulerTotalDispatches:  { offset: 21, type: 'counter',  unit: 'count', description: 'Total dispatch attempts' },
        preschedulerBypassed:         { offset: 22, type: 'counter',  unit: 'count', description: 'Messages sent directly from JS to scsynth, bypassing prescheduler (aggregate)' },
        preschedulerMaxLateMs:        { offset: 23, type: 'gauge',    unit: 'ms',    description: 'Maximum lateness at prescheduler (ms)' },

        // OSC Out metrics [24-25]
        oscOutMessagesSent:           { offset: 24, type: 'counter',  unit: 'count', description: 'OSC messages sent from JS to scsynth' },
        oscOutBytesSent:              { offset: 25, type: 'counter',  unit: 'bytes', description: 'Total bytes sent from JS to scsynth' },

        // OSC In metrics [26-29]
        oscInMessagesReceived:        { offset: 26, type: 'counter',  unit: 'count', description: 'OSC replies received from scsynth to JS' },
        oscInBytesReceived:           { offset: 27, type: 'counter',  unit: 'bytes', description: 'Total bytes received from scsynth to JS' },
        oscInMessagesDropped:         { offset: 28, type: 'counter',  unit: 'count', description: 'Replies lost in transit from scsynth to JS' },
        oscInCorrupted:               { offset: 29, type: 'counter',  unit: 'count', description: 'Corrupted messages detected from scsynth to JS' },

        // Debug metrics [30-31]
        debugMessagesReceived:        { offset: 30, type: 'counter',  unit: 'count', description: 'Debug messages from scsynth' },
        debugBytesReceived:           { offset: 31, type: 'counter',  unit: 'bytes', description: 'Debug bytes received' },

        // Ring buffer usage [32-37]
        inBufferUsedBytes:            { offset: 32, type: 'gauge',    unit: 'bytes', description: 'Bytes used in IN ring buffer' },
        outBufferUsedBytes:           { offset: 33, type: 'gauge',    unit: 'bytes', description: 'Bytes used in OUT ring buffer' },
        debugBufferUsedBytes:         { offset: 34, type: 'gauge',    unit: 'bytes', description: 'Bytes used in DEBUG ring buffer' },
        inBufferPeakBytes:            { offset: 35, type: 'gauge',    unit: 'bytes', description: 'Peak bytes used in IN ring buffer' },
        outBufferPeakBytes:           { offset: 36, type: 'gauge',    unit: 'bytes', description: 'Peak bytes used in OUT ring buffer' },
        debugBufferPeakBytes:         { offset: 37, type: 'gauge',    unit: 'bytes', description: 'Peak bytes used in DEBUG ring buffer' },

        // Bypass category metrics [38-41]
        bypassNonBundle:              { offset: 38, type: 'counter',  unit: 'count', description: 'Plain OSC messages (not bundles) that bypassed prescheduler' },
        bypassImmediate:              { offset: 39, type: 'counter',  unit: 'count', description: 'Bundles with timetag 0 or 1 that bypassed prescheduler' },
        bypassNearFuture:             { offset: 40, type: 'counter',  unit: 'count', description: 'Bundles within bypass lookahead threshold that bypassed prescheduler' },
        bypassLate:                   { offset: 41, type: 'counter',  unit: 'count', description: 'Timestamped OSC bundles arriving late into SuperSonic bypassing prescheduler' },

        // scsynth late timing diagnostics [42-44]
        scsynthSchedulerMaxLateMs:    { offset: 42, type: 'gauge',    unit: 'ms',    description: 'Maximum lateness observed in scsynth scheduler (ms)' },
        scsynthSchedulerLastLateMs:   { offset: 43, type: 'gauge',    unit: 'ms',    description: 'Most recent late magnitude in scsynth scheduler (ms)' },
        scsynthSchedulerLastLateTick: { offset: 44, type: 'gauge',    unit: 'count', description: 'Process count when last scsynth late occurred' },

        // Ring buffer direct write failures [45]
        ringBufferDirectWriteFails:   { offset: 45, type: 'counter',  unit: 'count', description: 'SAB mode only: optimistic direct writes attempted but failed due to ring buffer lock not being available (delivered via prescheduler instead)' },

        // Context metrics [46+] (main thread only)
        driftOffsetMs:                { offset: 46, type: 'gauge',    unit: 'ms',    signed: true, description: 'Clock drift between AudioContext and wall clock' },
        clockOffsetMs:                { offset: 47, type: 'gauge',    unit: 'ms',    signed: true, description: 'Clock offset for multi-system sync' },
        audioContextState:            { offset: 48, type: 'enum',     values: ['unknown', 'running', 'suspended', 'closed', 'interrupted'], description: 'AudioContext state' },
        bufferPoolUsedBytes:          { offset: 49, type: 'gauge',    unit: 'bytes', description: 'Buffer pool bytes used' },
        bufferPoolAvailableBytes:     { offset: 50, type: 'gauge',    unit: 'bytes', description: 'Buffer pool bytes available' },
        bufferPoolAllocations:        { offset: 51, type: 'counter',  unit: 'count', description: 'Total buffer allocations' },
        loadedSynthDefs:              { offset: 52, type: 'gauge',    unit: 'count', description: 'Number of loaded synthdefs' },
        scsynthSchedulerCapacity:     { offset: 53, type: 'constant', unit: 'count', description: 'Maximum scheduler queue size' },
        preschedulerCapacity:         { offset: 54, type: 'constant', unit: 'count', description: 'Maximum pending events in prescheduler' },
        inBufferCapacity:             { offset: 55, type: 'constant', unit: 'bytes', description: 'IN ring buffer capacity' },
        outBufferCapacity:            { offset: 56, type: 'constant', unit: 'bytes', description: 'OUT ring buffer capacity' },
        debugBufferCapacity:          { offset: 57, type: 'constant', unit: 'bytes', description: 'DEBUG ring buffer capacity' },
        mode:                         { offset: 58, type: 'enum',     values: ['sab', 'postMessage'], description: 'Transport mode' },
      },

      layout: {
        panels: [
          {
            title: 'OSC Out',
            rows: [
              { label: 'sent',   cells: [{ key: 'oscOutMessagesSent' }] },
              { label: 'bytes',  cells: [{ key: 'oscOutBytesSent', kind: 'muted', format: 'bytes' }] },
              { label: 'bypass', cells: [{ key: 'preschedulerBypassed', kind: 'green' }] },
              { label: 'lost',   cells: [{ key: 'scsynthSequenceGaps', kind: 'error' }] },
            ]
          },
          {
            title: 'Bypass',
            rows: [
              { label: 'msg',  cells: [{ key: 'bypassNonBundle', kind: 'muted' }] },
              { label: 'imm',  cells: [{ key: 'bypassImmediate', kind: 'muted' }] },
              { label: 'near', cells: [{ key: 'bypassNearFuture', kind: 'muted' }] },
              { label: 'late', cells: [{ key: 'bypassLate', kind: 'muted' }] },
            ]
          },
          {
            title: 'OSC In',
            rows: [
              { label: 'received',  cells: [{ key: 'oscInMessagesReceived' }] },
              { label: 'bytes',     cells: [{ key: 'oscInBytesReceived', kind: 'muted', format: 'bytes' }] },
              { label: 'dropped',   cells: [{ key: 'oscInMessagesDropped', kind: 'error' }] },
              { label: 'corrupted', cells: [{ key: 'oscInCorrupted', kind: 'error' }] },
            ]
          },
          {
            title: 'Presched Flow',
            rows: [
              { label: 'pending',    cells: [{ key: 'preschedulerPending' }, { sep: ' | ' }, { key: 'preschedulerPendingPeak', kind: 'muted' }] },
              { label: 'scheduled',  cells: [{ key: 'preschedulerBundlesScheduled' }] },
              { label: 'dispatched', cells: [{ key: 'preschedulerDispatched', kind: 'dim' }] },
              { label: 'min slack',  cells: [{ key: 'preschedulerMinHeadroomMs', kind: 'dim', format: 'headroom' }, { text: ' ms', kind: 'muted' }] },
            ]
          },
          {
            title: 'Presched Health',
            rows: [
              { label: 'lates',       cells: [{ key: 'preschedulerLates', kind: 'error' }, { sep: ' (' }, { key: 'preschedulerMaxLateMs', kind: 'dim' }, { text: 'ms max)', kind: 'muted' }] },
              { label: 'cancelled',   cells: [{ key: 'preschedulerEventsCancelled', kind: 'error' }] },
              { label: 'retried',     cells: [{ key: 'preschedulerMessagesRetried', kind: 'dim' }, { sep: ' | ' }, { key: 'preschedulerRetriesSucceeded', kind: 'green' }, { sep: ' | ' }, { key: 'preschedulerRetriesFailed', kind: 'error' }] },
              { label: 'retry queue', cells: [{ key: 'preschedulerRetryQueueSize' }, { sep: ' | ' }, { key: 'preschedulerRetryQueuePeak', kind: 'muted' }] },
            ]
          },
          {
            title: 'scsynth Scheduler',
            rows: [
              { label: 'queue',   cells: [{ key: 'scsynthSchedulerDepth' }, { sep: ' | ' }, { key: 'scsynthSchedulerPeakDepth', kind: 'muted' }] },
              { label: 'dropped', cells: [{ key: 'scsynthSchedulerDropped', kind: 'error' }] },
              { label: 'lates',   cells: [{ key: 'scsynthSchedulerLates', kind: 'error' }] },
              { label: 'max | last', cells: [{ key: 'scsynthSchedulerMaxLateMs', kind: 'error' }, { sep: ' | ' }, { key: 'scsynthSchedulerLastLateMs', kind: 'dim' }, { text: ' ms', kind: 'muted' }] },
            ]
          },
          {
            title: 'scsynth',
            rows: [
              { label: 'processed',   cells: [{ key: 'scsynthMessagesProcessed' }] },
              { label: 'dropped',     cells: [{ key: 'scsynthMessagesDropped', kind: 'error' }] },
              { label: 'synthdefs',   cells: [{ key: 'loadedSynthDefs' }] },
              { label: 'clock drift', cells: [{ key: 'driftOffsetMs', format: 'signed' }, { text: 'ms', kind: 'muted' }] },
            ]
          },
          {
            title: 'Ring Buffer Level',
            class: 'wide',
            rows: [
              { type: 'bar', label: 'in',  usedKey: 'inBufferUsedBytes',  peakKey: 'inBufferPeakBytes',  capacityKey: 'inBufferCapacity',  color: 'blue' },
              { type: 'bar', label: 'out', usedKey: 'outBufferUsedBytes', peakKey: 'outBufferPeakBytes', capacityKey: 'outBufferCapacity', color: 'green' },
              { type: 'bar', label: 'dbg', usedKey: 'debugBufferUsedBytes', peakKey: 'debugBufferPeakBytes', capacityKey: 'debugBufferCapacity', color: 'purple' },
              { label: 'direct write fails', cells: [{ key: 'ringBufferDirectWriteFails', kind: 'error' }] },
            ]
          },
          {
            title: 'AudioWorklet',
            rows: [
              { label: 'audio',       cells: [{ key: 'audioContextState', kind: 'green', format: 'enum' }] },
              { label: 'ticks',       cells: [{ key: 'scsynthProcessCount', kind: 'dim' }] },
              { label: 'WASM errors', cells: [{ key: 'scsynthWasmErrors', kind: 'error' }] },
              { label: 'debug',       cells: [{ key: 'debugMessagesReceived', kind: 'muted' }, { text: ' (' }, { key: 'debugBytesReceived', kind: 'muted', format: 'bytes' }, { text: ')' }] },
            ]
          },
          {
            title: 'Audio Buffers',
            rows: [
              { label: 'used',   cells: [{ key: 'bufferPoolUsedBytes', format: 'bytes' }] },
              { label: 'free',   cells: [{ key: 'bufferPoolAvailableBytes', kind: 'green', format: 'bytes' }] },
              { label: 'allocs', cells: [{ key: 'bufferPoolAllocations', kind: 'dim' }] },
            ]
          },
        ]
      },

      sentinels: {
        HEADROOM_UNSET: 0xFFFFFFFF,
      }
    });
  }

  /**
   * Get schema describing the node tree structure.
   * Useful for generating UIs or understanding tree data.
   */
  static getTreeSchema() {
    const nodeSchema = {
      id: { type: 'number', description: 'Unique node ID' },
      type: { type: 'string', values: ['group', 'synth'], description: 'Node type' },
      defName: { type: 'string', description: 'Synthdef name (synths only, empty for groups)' },
      children: { type: 'array', description: 'Child nodes (recursive)', itemSchema: '(self)' }
    };
    return {
      nodeCount: { type: 'number', description: 'Total nodes in tree' },
      version: { type: 'number', description: 'Increments on any tree change, useful for detecting updates' },
      droppedCount: { type: 'number', description: 'Nodes that exceeded mirror capacity (tree may be incomplete)' },
      root: {
        type: 'object',
        description: 'Root node of the tree (always a group with id 0)',
        schema: nodeSchema
      }
    };
  }

  static getRawTreeSchema() {
    return {
      nodeCount: { type: 'number', description: 'Total nodes in tree' },
      version: { type: 'number', description: 'Increments on any tree change, useful for detecting updates' },
      droppedCount: { type: 'number', description: 'Nodes that exceeded mirror capacity (tree may be incomplete)' },
      nodes: {
        type: 'array',
        description: 'Flat array of all nodes with internal linkage pointers',
        itemSchema: {
          id: { type: 'number', description: 'Unique node ID' },
          parentId: { type: 'number', description: 'Parent node ID (-1 for root)' },
          isGroup: { type: 'boolean', description: 'True if group, false if synth' },
          prevId: { type: 'number', description: 'Previous sibling node ID (-1 if none)' },
          nextId: { type: 'number', description: 'Next sibling node ID (-1 if none)' },
          headId: { type: 'number', description: 'First child node ID (groups only, -1 if empty)' },
          defName: { type: 'string', description: 'Synthdef name (synths only, empty for groups)' }
        }
      }
    };
  }

  // Private implementation
  #audioContext;
  #workletNode;
  #node = null;
  #osc;
  #wasmMemory;
  #bufferManager;
  #oscRewriter;
  #syncListeners;
  #sampleBaseURL;
  #synthdefBaseURL;
  #fetchRetryConfig;
  #assetLoader;
  #initialized;
  #initializing;
  #initPromise;
  #capabilities;
  #version;
  #config;

  // Extracted modules
  #eventEmitter;
  #metricsReader;
  #ntpTiming;
  #audioCapture;

  // Main thread OscChannel for sending OSC
  #oscChannel;

  // Track AudioContext state for recovery detection
  #previousAudioContextState = null;

  // Cached WASM bytes for fast recover()
  #cachedWasmBytes = null;

  // Snapshot tracking (postMessage mode)
  #snapshotsSent = 0;

  // Buffer for early debugRawBatch messages
  #earlyDebugMessages = [];
  #debugRawHandler = null;

  /**
   * Validate scsynthOptions (worldOptions) at construction time.
   * Throws descriptive errors for invalid configurations.
   * @param {Object} opts - The merged world options
   */
  #validateWorldOptions(opts) {
    // Helper to validate numeric option
    const validateNumber = (name, value, { min, max, allowZero = true } = {}) => {
      if (typeof value !== 'number' || !Number.isFinite(value)) {
        throw new Error(`scsynthOptions.${name} must be a finite number, got: ${value}`);
      }
      if (!allowZero && value === 0) {
        throw new Error(`scsynthOptions.${name} must be non-zero, got: ${value}`);
      }
      if (min !== undefined && value < min) {
        throw new Error(`scsynthOptions.${name} must be >= ${min}, got: ${value}`);
      }
      if (max !== undefined && value > max) {
        throw new Error(`scsynthOptions.${name} must be <= ${max}, got: ${value}`);
      }
    };

    // numBuffers: 1-65535
    validateNumber('numBuffers', opts.numBuffers, { min: 1, max: 65535 });

    // maxNodes: must be positive
    validateNumber('maxNodes', opts.maxNodes, { min: 1 });

    // maxGraphDefs: must be positive
    validateNumber('maxGraphDefs', opts.maxGraphDefs, { min: 1 });

    // maxWireBufs: must be positive
    validateNumber('maxWireBufs', opts.maxWireBufs, { min: 1 });

    // numAudioBusChannels: must be positive
    validateNumber('numAudioBusChannels', opts.numAudioBusChannels, { min: 1 });

    // numInputBusChannels: must be non-negative
    validateNumber('numInputBusChannels', opts.numInputBusChannels, { min: 0 });

    // numOutputBusChannels: must be 1-128 (C++ static_audio_bus is float[128*128])
    validateNumber('numOutputBusChannels', opts.numOutputBusChannels, { min: 1, max: 128 });

    // numControlBusChannels: must be positive
    validateNumber('numControlBusChannels', opts.numControlBusChannels, { min: 1 });

    // bufLength: must be exactly 128 (WebAudio API constraint)
    if (opts.bufLength !== 128) {
      throw new Error(`scsynthOptions.bufLength must be 128 (WebAudio API constraint), got: ${opts.bufLength}`);
    }

    // realTimeMemorySize: must be positive
    validateNumber('realTimeMemorySize', opts.realTimeMemorySize, { min: 1 });

    // numRGens: must be positive
    validateNumber('numRGens', opts.numRGens, { min: 1 });

    // realTime: must be boolean
    if (typeof opts.realTime !== 'boolean') {
      throw new Error(`scsynthOptions.realTime must be a boolean, got: ${typeof opts.realTime}`);
    }

    // memoryLocking: must be boolean
    if (typeof opts.memoryLocking !== 'boolean') {
      throw new Error(`scsynthOptions.memoryLocking must be a boolean, got: ${typeof opts.memoryLocking}`);
    }

    // loadGraphDefs: 0 or 1
    if (opts.loadGraphDefs !== 0 && opts.loadGraphDefs !== 1) {
      throw new Error(`scsynthOptions.loadGraphDefs must be 0 or 1, got: ${opts.loadGraphDefs}`);
    }

    // preferredSampleRate: 0 (auto) or valid range (8000-384000)
    validateNumber('preferredSampleRate', opts.preferredSampleRate, { min: 0, max: 384000 });
    if (opts.preferredSampleRate !== 0 && opts.preferredSampleRate < 8000) {
      throw new Error(`scsynthOptions.preferredSampleRate must be 0 (auto) or >= 8000, got: ${opts.preferredSampleRate}`);
    }

    // verbosity: 0-4
    validateNumber('verbosity', opts.verbosity, { min: 0, max: 4 });
  }

  constructor(options = {}) {
    this.#initialized = false;
    this.#initializing = false;
    this.#initPromise = null;
    this.#capabilities = {};
    this.#version = null;

    // Initialize extracted modules
    this.#eventEmitter = new EventEmitter();
    this.#metricsReader = new MetricsReader({ mode: options.mode || 'postMessage' });
    this.#audioCapture = new AudioCapture({});

    // Core components
    this.#audioContext = null;
    this.#workletNode = null;
    this.#osc = null;
    this.#bufferManager = null;
    this.loadedSynthDefs = new Map();

    // Configuration
    // coreBaseURL is for WASM and workers (from supersonic-scsynth-core package)
    // baseURL is a convenience shorthand when all assets are co-located
    const baseURL = options.baseURL || null;
    const coreBaseURL = options.coreBaseURL || baseURL;
    const workerBaseURL = options.workerBaseURL || (coreBaseURL ? `${coreBaseURL}workers/` : null);
    const wasmBaseURL = options.wasmBaseURL || (coreBaseURL ? `${coreBaseURL}wasm/` : null);

    if (!workerBaseURL || !wasmBaseURL) {
      throw new Error(
        `SuperSonic requires explicit URL configuration.\n\n` +
        `For CDN usage:\n` +
        `  import { SuperSonic } from 'https://unpkg.com/supersonic-scsynth@VERSION/dist/supersonic.js';\n` +
        `  new SuperSonic({ baseURL: 'https://unpkg.com/supersonic-scsynth@VERSION/dist/' })\n\n` +
        `For local usage:\n` +
        `  new SuperSonic({ baseURL: '/path/to/supersonic/dist/' })\n\n` +
        `See: https://github.com/samaaron/supersonic#configuration`
      );
    }

    const worldOptions = { ...defaultWorldOptions, ...options.scsynthOptions };
    this.#validateWorldOptions(worldOptions);
    const mode = options.mode || 'postMessage';

    this.#config = {
      mode: mode,
      snapshotIntervalMs: options.snapshotIntervalMs ?? SNAPSHOT_INTERVAL_MS,
      wasmUrl: options.wasmUrl || wasmBaseURL + "scsynth-nrt.wasm",
      wasmBaseURL: wasmBaseURL,
      workletUrl: options.workletUrl || workerBaseURL + "scsynth_audio_worklet.js",
      workerBaseURL: workerBaseURL,
      audioContext: options.audioContext || null,
      autoConnect: options.autoConnect !== false,
      audioContextOptions: {
        latencyHint: "interactive",
        sampleRate: 48000,
        ...options.audioContextOptions,
      },
      memory: MemoryLayout,
      worldOptions: worldOptions,
      preschedulerCapacity: options.preschedulerCapacity || 65536,
      bypassLookaheadMs: options.bypassLookaheadMs ?? 500,
      activityEvent: {
        maxLineLength: options.activityEvent?.maxLineLength ?? 200,
        scsynthMaxLineLength: options.activityEvent?.scsynthMaxLineLength ?? null,
        oscInMaxLineLength: options.activityEvent?.oscInMaxLineLength ?? null,
        oscOutMaxLineLength: options.activityEvent?.oscOutMaxLineLength ?? null,
      },
      debug: options.debug ?? false,
      debugScsynth: options.debugScsynth ?? false,
      debugOscIn: options.debugOscIn ?? false,
      debugOscOut: options.debugOscOut ?? false,
      activityConsoleLog: {
        maxLineLength: options.activityConsoleLog?.maxLineLength ?? 200,
        scsynthMaxLineLength: options.activityConsoleLog?.scsynthMaxLineLength ?? null,
        oscInMaxLineLength: options.activityConsoleLog?.oscInMaxLineLength ?? null,
        oscOutMaxLineLength: options.activityConsoleLog?.oscOutMaxLineLength ?? null,
      },
    };

    this.#sampleBaseURL = options.sampleBaseURL || (baseURL ? `${baseURL}samples/` : null);
    this.#synthdefBaseURL = options.synthdefBaseURL || (baseURL ? `${baseURL}synthdefs/` : null);

    this.#fetchRetryConfig = {
      maxRetries: options.fetchMaxRetries ?? 3,
      baseDelay: options.fetchRetryDelay ?? 1000,
    };

    this.#assetLoader = new AssetLoader({
      onLoadingEvent: (event, data) => this.#eventEmitter.emit(event, data),
      maxRetries: this.#fetchRetryConfig.maxRetries,
      baseDelay: this.#fetchRetryConfig.baseDelay,
    });

    this.bootStats = {
      initStartTime: null,
      initDuration: null,
    };
  }

  // ============================================================================
  // PUBLIC GETTERS
  // ============================================================================

  get initialized() { return this.#initialized; }
  get initializing() { return this.#initializing; }
  get audioContext() { return this.#audioContext; }
  get mode() { return this.#config.mode; }
  get bufferConstants() { return this.#metricsReader.bufferConstants; }
  get ringBufferBase() { return this.#metricsReader.ringBufferBase; }
  get sharedBuffer() { return this.#metricsReader.sharedBuffer; }
  get node() { return this.#node; }
  get osc() { return this.#osc; }

  // ============================================================================
  // EVENT EMITTER DELEGATION
  // ============================================================================

  on(event, callback) { return this.#eventEmitter.on(event, callback); }
  off(event, callback) { this.#eventEmitter.off(event, callback); return this; }
  once(event, callback) { this.#eventEmitter.once(event, callback); return this; }
  removeAllListeners(event) { this.#eventEmitter.removeAllListeners(event); return this; }

  // ============================================================================
  // INITIALIZATION
  // ============================================================================

  async init() {
    if (this.#initialized) return;
    if (this.#initPromise) return this.#initPromise;

    this.#initPromise = this.#doInit();
    return this.#initPromise;
  }

  async #doInit() {
    this.#initializing = true;
    this.bootStats.initStartTime = performance.now();

    try {
      this.#setAndValidateCapabilities();
      this.#initializeMemory();
      this.#initializeAudioContext();
      this.#initializeBufferManager();
      this.#initializeOSCRewriter();
      const wasmBytes = await this.#loadWasm();
      await this.#initializeAudioWorklet(wasmBytes);
      await this.#initializeOSC();
      await this.#finishInitialization();
    } catch (error) {
      this.#initializing = false;
      this.#initPromise = null;
      console.error("[SuperSonic] Initialization failed:", error);
      this.#eventEmitter.emit('error', error);
      throw error;
    }
  }

  // ============================================================================
  // METRICS API
  // ============================================================================

  getMetrics() {
    return this.#gatherMetrics();
  }

  /**
   * Get metrics as a flat Uint32Array for zero-allocation reading.
   * Returns the same array reference every call — values are updated in-place.
   * Slots 0-45: SAB/snapshot metrics, 46+: context metrics.
   * Use getMetricsSchema().metrics for offset mappings.
   * @returns {Uint32Array}
   */
  getMetricsArray() {
    this.#updateMergedArray();
    return this.#metricsReader.getMergedArray();
  }

  /**
   * Get a diagnostic snapshot containing metrics, node tree, and memory info.
   * Useful for debugging timing issues, capturing state for bug reports, etc.
   * @returns {Object} Snapshot with timestamp, metrics (with descriptions), nodeTree, and memory info
   */
  getSnapshot() {
    const rawMetrics = this.#gatherMetrics();
    const schemaMetrics = SuperSonic.getMetricsSchema()?.metrics || {};

    // Build metrics with descriptions
    const metricsWithDescriptions = {};
    for (const [key, value] of Object.entries(rawMetrics)) {
      const def = schemaMetrics[key];
      if (def?.description) {
        metricsWithDescriptions[key] = {
          value,
          description: def.description,
        };
      } else {
        metricsWithDescriptions[key] = { value };
      }
    }

    // Get JS heap memory info (Chrome only, non-standard API)
    let memory = null;
    if (typeof performance !== 'undefined' && performance.memory) {
      memory = {
        usedJSHeapSize: performance.memory.usedJSHeapSize,
        totalJSHeapSize: performance.memory.totalJSHeapSize,
        jsHeapSizeLimit: performance.memory.jsHeapSizeLimit,
      };
    }

    return {
      timestamp: new Date().toISOString(),
      metrics: metricsWithDescriptions,
      nodeTree: this.getRawTree(),
      memory,
    };
  }

  // ============================================================================
  // TIMING API
  // ============================================================================

  /**
   * Set clock offset for multi-system sync (e.g., Ableton Link, NTP server).
   * This shifts all scheduled bundle execution times by the specified offset.
   * Positive values mean the shared/server clock is ahead of local time —
   * bundles with shared-clock timetags are shifted earlier to compensate.
   * @param {number} offsetS - Offset in seconds
   */
  setClockOffset(offsetS) {
    this.#ensureInitialized('set clock offset');
    this.#ntpTiming?.setClockOffset(offsetS);
  }

  // ============================================================================
  // RECOVERY API
  // ============================================================================

  /**
   * Smart recovery - tries quick resume first, falls back to full reload.
   * Use this when you don't know if a quick resume will work.
   * @returns {Promise<boolean>} true if audio is running after recovery
   */
  async recover() {
    if (!this.#initialized) return false;

    if (__DEV__) console.log('[Dbg-SuperSonic] Attempting recovery...');

    if (await this.resume()) {
      if (__DEV__) console.log('[Dbg-SuperSonic] Quick resume succeeded');
      return true;
    }

    if (__DEV__) console.log('[Dbg-SuperSonic] Resume failed, doing full reload');
    return await this.reload();
  }

  /**
   * Quick resume - just resumes AudioContext and resyncs timing.
   * Memory and node tree are preserved. Does NOT emit 'setup' event.
   * Use when you know the worklet is still running (e.g., tab was just backgrounded briefly).
   * @returns {Promise<boolean>} true if worklet is running after resume
   */
  async resume() {
    if (!this.#initialized || !this.#audioContext) return false;

    // Clear stale messages before resuming so scheduled events from
    // before the suspend (e.g. fade-outs) don't interfere with new work
    await this.purge();

    try {
      await this.#audioContext.resume();
    } catch (e) {
      // Resume may fail
    }

    this.#ntpTiming?.startDriftTimer();

    const count1 = this.#readProcessCount();
    if (count1 === null) {
      // No metrics available yet — check AudioContext state instead
      const isRunning = this.#audioContext.state === 'running';
      if (isRunning) {
        this.#ntpTiming?.resync();
        this.#eventEmitter.emit('resumed');
      }
      return isRunning;
    }

    await new Promise(resolve => setTimeout(resolve, 200));
    const count2 = this.#readProcessCount();

    const isRunning = count2 !== null && count2 > count1;
    if (isRunning) {
      this.#ntpTiming?.resync();
      this.#eventEmitter.emit('resumed');
    }

    return isRunning;
  }

  /**
   * Suspend the AudioContext and stop the drift timer.
   * The worklet remains loaded but processing stops.
   * The audiocontext statechange listener handles emitting events.
   */
  async suspend() {
    if (!this.#initialized) return;
    this.#ntpTiming?.stopDriftTimer();
    try {
      await this.#audioContext?.suspend();
    } catch (e) {
      // Suspend may fail
    }
  }

  /**
   * Full reload - destroys and recreates worklet/WASM, restores synthdefs and buffers.
   * Emits 'setup' event so you can rebuild groups, FX chains, bus routing.
   * Use when the worklet was killed (e.g., long background, browser reclaimed memory).
   * @returns {Promise<boolean>} true if reload succeeded
   */
  async reload() {
    if (!this.#initialized) return false;

    this.#eventEmitter.emit('reload:start');

    const cachedSynthDefs = new Map(this.loadedSynthDefs);
    const cachedBuffers = this.#bufferManager?.getAllocatedBuffers() || [];

    await this.#partialShutdown();
    await this.#partialInit();

    // Restore synthdefs
    for (const [name, data] of cachedSynthDefs) {
      try {
        await this.send('/d_recv', data);
      } catch (e) {
        console.error(`[SuperSonic] Failed to restore synthdef ${name}:`, e);
      }
    }

    // Restore buffers
    for (const buf of cachedBuffers) {
      try {
        if (this.#config.mode === 'postMessage' && buf.source) {
          if (buf.source.type === 'file') {
            await this.loadSample(buf.bufnum, buf.source.path, buf.source.startFrame || 0, buf.source.numFrames || 0);
          }
        } else {
          const uuid = crypto.randomUUID();
          await this.send('/b_allocPtr', buf.bufnum, buf.ptr, buf.numFrames, buf.numChannels, buf.sampleRate, uuid);
        }
      } catch (e) {
        console.error(`[SuperSonic] Failed to restore buffer ${buf.bufnum}:`, e);
      }
    }

    if (cachedSynthDefs.size > 0 || cachedBuffers.length > 0) {
      await this.sync();
    }

    this.#eventEmitter.emit('reload:complete', { success: true });
    return true;
  }

  async #partialShutdown() {
    this.#ntpTiming?.stopDriftTimer();
    this.#syncListeners?.clear();
    this.#syncListeners = null;

    if (this.#osc) {
      this.#osc.cancelAll();
      this.#osc.dispose();
      this.#osc = null;
    }

    if (this.#workletNode) {
      this.#workletNode.disconnect();
      this.#workletNode = null;
    }

    if (this.#audioContext) {
      await this.#audioContext.close();
      this.#audioContext = null;
    }

    this.#initialized = false;
    this.loadedSynthDefs.clear();
    this.#initPromise = null;
    this.#oscChannel = null;
    this.#ntpTiming?.reset();
  }

  async #partialInit() {
    this.#initializing = true;
    this.bootStats.initStartTime = performance.now();

    try {
      this.#initializeAudioContext();
      if (this.#bufferManager) {
        this.#bufferManager.updateAudioContext(this.#audioContext);
      }
      this.#initializeOSCRewriter();
      const wasmBytes = await this.#loadWasm();
      await this.#initializeAudioWorklet(wasmBytes);
      await this.#initializeOSC();
      await this.#finishInitialization();
    } catch (error) {
      this.#initializing = false;
      this.#initPromise = null;
      console.error("[SuperSonic] Partial init failed:", error);
      this.#eventEmitter.emit('error', error);
      throw error;
    }
  }

  // ============================================================================
  // NODE TREE API
  // ============================================================================

  getRawTree() {
    if (!this.#initialized) {
      return { nodeCount: 0, version: 0, droppedCount: 0, nodes: [] };
    }

    const bc = this.#metricsReader.bufferConstants;
    if (!bc) {
      return { nodeCount: 0, version: 0, droppedCount: 0, nodes: [] };
    }

    let buffer, treeOffset;
    if (this.#config.mode === 'postMessage') {
      const snapshot = this.#metricsReader.getSnapshotBuffer();
      if (!snapshot) {
        return { nodeCount: 0, version: 0, droppedCount: 0, nodes: [] };
      }
      buffer = snapshot;
      treeOffset = bc.METRICS_SIZE;
    } else {
      const sab = this.#metricsReader.sharedBuffer;
      if (!sab) {
        return { nodeCount: 0, version: 0, droppedCount: 0, nodes: [] };
      }
      buffer = sab;
      treeOffset = this.#metricsReader.ringBufferBase + bc.NODE_TREE_START;
    }

    return parseNodeTree(buffer, treeOffset, bc);
  }

  getTree() {
    const raw = this.getRawTree();

    // Build hierarchical tree from flat node list
    const buildNode = (rawNode) => ({
      id: rawNode.id,
      type: rawNode.isGroup ? 'group' : 'synth',
      defName: rawNode.defName,
      children: []
    });

    // Create node map for efficient lookup
    const nodeMap = new Map();
    for (const rawNode of raw.nodes) {
      nodeMap.set(rawNode.id, buildNode(rawNode));
    }

    // Build parent-child relationships
    let root = null;
    for (const rawNode of raw.nodes) {
      const node = nodeMap.get(rawNode.id);
      if (rawNode.parentId === -1 || rawNode.parentId === 0 && rawNode.id === 0) {
        // Root node (id 0 with parentId -1 or 0)
        root = node;
      } else {
        const parent = nodeMap.get(rawNode.parentId);
        if (parent) {
          parent.children.push(node);
        }
      }
    }

    return {
      nodeCount: raw.nodeCount,
      version: raw.version,
      droppedCount: raw.droppedCount,
      root: root || { id: 0, type: 'group', defName: '', children: [] }
    };
  }

  // ============================================================================
  // AUDIO CAPTURE API
  // ============================================================================

  startCapture() {
    this.#ensureInitialized("start capture");
    this.#audioCapture.start();
  }

  stopCapture() {
    this.#ensureInitialized("stop capture");
    return this.#audioCapture.stop();
  }

  isCaptureEnabled() {
    return this.#audioCapture.isEnabled();
  }

  getCaptureFrames() {
    return this.#audioCapture.getFrameCount();
  }

  getMaxCaptureDuration() {
    return this.#audioCapture.getMaxDuration();
  }

  // ============================================================================
  // OSC MESSAGING API
  // ============================================================================

  async send(address, ...args) {
    this.#ensureInitialized("send OSC messages");

    // Block unsupported commands
    const blocked = {
      "/d_load": "Use loadSynthDef() or send /d_recv with synthdef bytes instead.",
      "/d_loadDir": "Use loadSynthDef() or send /d_recv with synthdef bytes instead.",
      "/b_read": "Use loadSample() to load audio into a buffer.",
      "/b_readChannel": "Use loadSample() to load audio into a buffer.",
      "/b_write": "Writing audio files is not available in the browser.",
      "/b_close": "Writing audio files is not available in the browser.",
      "/clearSched": "Use purge() to clear both the JS prescheduler and WASM scheduler.",
      "/error": "SuperSonic always enables error notifications so you never miss a /fail message.",
    };

    if (blocked[address]) {
      throw new Error(`${address} is not supported in SuperSonic. ${blocked[address]}`);
    }

    // Cache synthdefs for /d_recv
    if (address === "/d_recv") {
      const synthdefBytes = args[0];
      if (synthdefBytes instanceof Uint8Array || synthdefBytes instanceof ArrayBuffer) {
        const bytes = synthdefBytes instanceof ArrayBuffer ? new Uint8Array(synthdefBytes) : synthdefBytes;
        const name = extractSynthDefName(bytes) || 'unknown';
        this.loadedSynthDefs.set(name, bytes);
      }
    }

    // Track synthdef frees
    if (address === "/d_free") {
      for (const name of args) {
        if (typeof name === "string") {
          this.loadedSynthDefs.delete(name);
        }
      }
    } else if (address === "/d_freeAll") {
      this.loadedSynthDefs.clear();
    }

    // Normalize ArrayBuffer to Uint8Array for blob args
    const normalizedArgs = args.map(arg => {
      if (arg instanceof ArrayBuffer) return new Uint8Array(arg);
      return arg;
    });

    const oscData = SuperSonic.osc.encodeMessage(address, normalizedArgs);

    if (this.#config.debug || this.#config.debugOscOut) {
      const maxLen = this.#config.activityConsoleLog.oscOutMaxLineLength ?? this.#config.activityConsoleLog.maxLineLength;
      const argsStr = args.map(a => {
        if (a instanceof Uint8Array || a instanceof ArrayBuffer) return `<${a.byteLength || a.length} bytes>`;
        const str = JSON.stringify(a);
        return str.length > maxLen ? str.slice(0, maxLen) + '...' : str;
      }).join(', ');
      console.log(`[OSC →] ${address}${argsStr ? ' ' + argsStr : ''}`);
    }

    return this.sendOSC(oscData);
  }

  async sendOSC(oscData, options = {}) {
    this.#ensureInitialized("send OSC data");

    const uint8Data = this.#toUint8Array(oscData);
    const preparedData = await this.#prepareOutboundPacket(uint8Data);

    // Note: message:sent is now emitted via the centralized OSC log from the worklet
    // This ensures all messages (from main thread and workers) are captured

    // Classify the message to determine routing
    const category = this.#oscChannel.classify(preparedData);

    if (shouldBypass(category)) {
      // Bypass: send direct to worklet
      if (this.#config.mode === 'sab') {
        // SAB mode: use OscChannel for direct ring buffer write
        this.#oscChannel.send(preparedData);
        // OscChannel writes metrics directly to shared memory
      } else {
        // PM mode: use transport's sendImmediate which tracks metrics locally
        this.#osc.sendImmediate(preparedData, category);
      }
    } else {
      // Far-future: goes to prescheduler for timing
      // Check size limit: WASM scheduler has fixed slot size
      const slotSize = this.#metricsReader.bufferConstants?.scheduler_slot_size;
      if (slotSize && preparedData.length > slotSize) {
        throw new Error(
          `OSC bundle too large to schedule (${preparedData.length} > ${slotSize} bytes). ` +
          `Use immediate timestamp (0 or 1) for large messages, or reduce bundle size.`
        );
      }
      // Send to prescheduler with session/tag options for cancellation
      this.#osc.sendWithOptions(preparedData, options);
    }
  }

  cancelTag(runTag) {
    this.#ensureInitialized("cancel by tag");
    this.#osc.cancelTag(runTag);
  }

  cancelSession(sessionId) {
    this.#ensureInitialized("cancel by session");
    this.#osc.cancelSession(sessionId);
  }

  cancelSessionTag(sessionId, runTag) {
    this.#ensureInitialized("cancel by session and tag");
    this.#osc.cancelSessionTag(sessionId, runTag);
  }

  cancelAll() {
    this.#ensureInitialized("cancel all scheduled");
    this.#osc.cancelAll();
  }

  /**
   * Flush all pending OSC messages from both the JS prescheduler
   * and the WASM BundleScheduler.
   *
   * Unlike cancelAll() which only clears the JS prescheduler,
   * this also clears bundles that have already been consumed from the
   * ring buffer and are sitting in the WASM scheduler's priority queue.
   *
   * Uses a postMessage flag (not the ring buffer) to avoid the race
   * condition where stale scheduled bundles would fire before a
   * /clearSched command could be read from the ring buffer.
   *
   * Returns a promise that resolves when both the prescheduler and
   * WASM scheduler have confirmed they are cleared.
   *
   * @returns {Promise<void>}
   */
  async purge() {
    this.#ensureInitialized("purge");

    const preschedulerDone = this.#osc.cancelAllWithAck();

    const workletDone = new Promise(resolve => {
      const handler = (event) => {
        if (event.data.type === 'clearSchedAck') {
          this.#workletNode.port.removeEventListener('message', handler);
          resolve();
        }
      };
      this.#workletNode.port.addEventListener('message', handler);
      this.#workletNode.port.postMessage({ type: 'clearSched', ack: true });
    });

    await Promise.all([preschedulerDone, workletDone]);
  }

  /**
   * Create an OscChannel for direct worker-to-worklet communication
   *
   * Returns an OscChannel that can be transferred to a Web Worker,
   * allowing that worker to send OSC messages directly to the AudioWorklet
   * without going through the main thread.
   *
   * In SAB mode: Returns a channel backed by SharedArrayBuffer (ring buffer writes)
   * In postMessage mode: Returns a channel backed by MessagePort
   *
   * Usage:
   *   const channel = supersonic.createOscChannel();
   *   myWorker.postMessage({ channel: channel.transferable }, channel.transferList);
   *
   * In worker:
   *   import { OscChannel } from 'supersonic-scsynth';
   *   const channel = OscChannel.fromTransferable(event.data.channel);
   *   channel.send(oscBytes);
   *
   * @returns {OscChannel}
   */
  createOscChannel(options = {}) {
    this.#ensureInitialized("create OSC channel");
    return this.#osc.createOscChannel(options);
  }

  // ============================================================================
  // ASSET LOADING API
  // ============================================================================

  async loadSynthDef(source) {
    this.#ensureInitialized("load synthdef");

    let synthdefData;
    let synthName;

    if (typeof source === 'string') {
      // Name or path/URL string
      let path;
      if (this.#looksLikePathOrURL(source)) {
        path = source;
      } else {
        if (!this.#synthdefBaseURL) {
          throw new Error("synthdefBaseURL not configured.");
        }
        path = `${this.#synthdefBaseURL}${source}.scsyndef`;
      }

      // Extract name from path for loading event
      const pathName = extractSynthDefName(path);

      const arrayBuffer = await this.#assetLoader.fetch(path, { type: 'synthdef', name: pathName });
      synthdefData = new Uint8Array(arrayBuffer);

      // Extract actual name from binary (more reliable than filename)
      synthName = extractSynthDefName(synthdefData) || pathName;

    } else if (source instanceof ArrayBuffer || ArrayBuffer.isView(source)) {
      // Raw bytes (ArrayBuffer or TypedArray like Uint8Array)
      synthdefData = source instanceof ArrayBuffer
        ? new Uint8Array(source)
        : new Uint8Array(source.buffer, source.byteOffset, source.byteLength);

      synthName = extractSynthDefName(synthdefData);
      if (!synthName) {
        throw new Error('Could not extract synthdef name from binary data. Make sure it\'s a valid .scsyndef file.');
      }

    } else if (source instanceof Blob) {
      // File or Blob
      const arrayBuffer = await source.arrayBuffer();
      synthdefData = new Uint8Array(arrayBuffer);

      synthName = extractSynthDefName(synthdefData);
      if (!synthName) {
        throw new Error('Could not extract synthdef name from file. Make sure it\'s a valid .scsyndef file.');
      }

    } else {
      throw new Error('loadSynthDef source must be a name, path/URL string, ArrayBuffer, Uint8Array, or File/Blob');
    }

    await this.send("/d_recv", synthdefData);

    return { name: synthName, size: synthdefData.length };
  }

  async loadSynthDefs(names) {
    this.#ensureInitialized("load synthdefs");

    const results = {};
    await Promise.all(
      names.map(async (name) => {
        try {
          await this.loadSynthDef(name);
          results[name] = { success: true };
        } catch (error) {
          results[name] = { success: false, error: error.message };
        }
      })
    );
    return results;
  }

  async loadSample(bufnum, source, startFrame = 0, numFrames = 0) {
    this.#ensureInitialized("load samples");

    let bufferInfo;

    if (typeof source === 'string') {
      // Path or URL
      bufferInfo = await this.#bufferManager.prepareFromFile({
        bufnum, path: source, startFrame, numFrames,
      });
    } else if (source instanceof ArrayBuffer || ArrayBuffer.isView(source)) {
      // ArrayBuffer or TypedArray
      bufferInfo = await this.#bufferManager.prepareFromBlob({
        bufnum, blob: source, startFrame, numFrames,
      });
    } else if (source instanceof Blob) {
      // File or Blob - read into ArrayBuffer first
      const arrayBuffer = await source.arrayBuffer();
      bufferInfo = await this.#bufferManager.prepareFromBlob({
        bufnum, blob: arrayBuffer, startFrame, numFrames,
      });
    } else {
      throw new Error('loadSample source must be a path/URL string, ArrayBuffer, TypedArray, or File/Blob');
    }

    await this.send(
      "/b_allocPtr", bufnum, bufferInfo.ptr, bufferInfo.numFrames,
      bufferInfo.numChannels, bufferInfo.sampleRate, bufferInfo.uuid
    );

    return await bufferInfo.allocationComplete;
  }

  getLoadedBuffers() {
    this.#ensureInitialized("get loaded buffers");

    const buffers = this.#bufferManager?.getAllocatedBuffers() || [];
    return buffers.map(({ bufnum, numFrames, numChannels, sampleRate, source }) => ({
      bufnum,
      numFrames,
      numChannels,
      sampleRate,
      source: source?.path || source?.name || null,
      duration: sampleRate > 0 ? numFrames / sampleRate : 0,
    }));
  }

  async sync(syncId = Math.floor(Math.random() * 2147483647)) {
    this.#ensureInitialized("sync");

    const syncPromise = new Promise((resolve, reject) => {
      const timeout = setTimeout(() => {
        this.#syncListeners?.delete(syncId);
        reject(new Error("Timeout waiting for /synced response"));
      }, SYNC_TIMEOUT_MS);

      const messageHandler = () => {
        clearTimeout(timeout);
        this.#syncListeners.delete(syncId);
        resolve();
      };

      if (!this.#syncListeners) this.#syncListeners = new Map();
      this.#syncListeners.set(syncId, messageHandler);
    });

    await this.send("/sync", syncId);
    await syncPromise;

    if (this.#config.mode === 'postMessage') {
      await new Promise(r => setTimeout(r, this.#config.snapshotIntervalMs * 2));
    }
  }

  // ============================================================================
  // INFO API
  // ============================================================================

  getInfo() {
    this.#ensureInitialized("get info");

    return {
      sampleRate: this.#audioContext.sampleRate,
      numBuffers: this.#config.worldOptions.numBuffers,
      totalMemory: this.#config.memory.totalMemory,
      wasmHeapSize: this.#config.memory.wasmHeapSize,
      bufferPoolSize: this.#config.memory.bufferPoolSize,
      bootTimeMs: this.bootStats.initDuration,
      capabilities: { ...this.#capabilities },
      version: this.#version,
    };
  }

  // ============================================================================
  // LIFECYCLE API
  // ============================================================================

  async shutdown() {
    if (!this.#initialized && !this.#initializing) return;

    this.#eventEmitter.emit("shutdown");
    this.#ntpTiming?.stopDriftTimer();
    this.#syncListeners?.clear();
    this.#syncListeners = null;

    if (this.#osc) {
      this.#osc.cancelAll();
      this.#osc.dispose();
      this.#osc = null;
    }

    if (this.#workletNode) {
      this.#workletNode.disconnect();
      this.#workletNode = null;
    }

    if (this.#audioContext) {
      await this.#audioContext.close();
      this.#audioContext = null;
    }

    if (this.#bufferManager) {
      this.#bufferManager.destroy();
      this.#bufferManager = null;
    }

    this.#oscRewriter = null;
    this.#oscChannel = null;
    this.#initialized = false;
    this.loadedSynthDefs.clear();
    this.#initPromise = null;
    this.#wasmMemory = null;
    this.#ntpTiming?.reset();
    this.bootStats = { initStartTime: null, initDuration: null };
  }

  async destroy() {
    this.#eventEmitter.emit("destroy");
    await this.shutdown();
    this.#cachedWasmBytes = null;
    this.#eventEmitter.clearAllListeners();
  }

  async reset() {
    await this.shutdown();
    await this.init();
  }

  // ============================================================================
  // PRIVATE: INITIALIZATION HELPERS
  // ============================================================================

  #setAndValidateCapabilities() {
    this.#capabilities = {
      audioWorklet: typeof AudioWorklet !== "undefined",
      sharedArrayBuffer: typeof SharedArrayBuffer !== "undefined",
      crossOriginIsolated: window.crossOriginIsolated === true,
      atomics: typeof Atomics !== "undefined",
      webWorker: typeof Worker !== "undefined",
    };

    const mode = this.#config.mode;
    const required = ["audioWorklet", "webWorker"];

    if (mode === 'sab') {
      required.push("sharedArrayBuffer", "crossOriginIsolated", "atomics");
    }

    const missing = required.filter((f) => !this.#capabilities[f]);

    if (missing.length > 0) {
      const error = new Error(`Missing required features for ${mode} mode: ${missing.join(", ")}`);
      if (mode === 'sab' && !this.#capabilities.crossOriginIsolated) {
        error.message += "\n\nConsider using mode: 'postMessage' which doesn't require COOP/COEP headers.";
      }
      throw error;
    }

    if (mode !== 'sab' && mode !== 'postMessage') {
      throw new Error(`Invalid mode: '${mode}'. Use 'sab' or 'postMessage'.`);
    }
  }

  #initializeMemory() {
    const memConfig = this.#config.memory;
    const mode = this.#config.mode;

    if (mode === 'sab') {
      this.#wasmMemory = new WebAssembly.Memory({
        initial: memConfig.totalPages,
        maximum: memConfig.totalPages,
        shared: true,
      });
    } else {
      this.#wasmMemory = null;
    }
  }

  #initializeAudioContext() {
    if (this.#config.audioContext) {
      this.#audioContext = this.#config.audioContext;
    } else {
      this.#audioContext = new AudioContext(this.#config.audioContextOptions);
    }

    this.#audioContext.addEventListener('statechange', () => {
      const state = this.#audioContext?.state;
      if (!state) return;

      const previousState = this.#previousAudioContextState;
      this.#previousAudioContextState = state;

      if (state === 'running' && (previousState === 'suspended' || previousState === 'interrupted')) {
        this.#ntpTiming?.resync();
      }

      this.#eventEmitter.emit('audiocontext:statechange', { state });
      if (state === 'suspended') this.#eventEmitter.emit('audiocontext:suspended');
      else if (state === 'running') this.#eventEmitter.emit('audiocontext:resumed');
      else if (state === 'interrupted') this.#eventEmitter.emit('audiocontext:interrupted');
    });
  }

  #initializeBufferManager() {
    const sharedBuffer = this.#config.mode === 'sab' ? this.#wasmMemory.buffer : null;

    this.#bufferManager = new BufferManager({
      mode: this.#config.mode,
      audioContext: this.#audioContext,
      sharedBuffer: sharedBuffer,
      bufferPoolConfig: {
        start: this.#config.memory.bufferPoolOffset,
        size: this.#config.memory.bufferPoolSize,
      },
      sampleBaseURL: this.#sampleBaseURL,
      maxBuffers: this.#config.worldOptions.numBuffers,
      assetLoader: this.#assetLoader,
    });
  }

  #initializeOSCRewriter() {
    this.#oscRewriter = new OSCRewriter({
      bufferManager: this.#bufferManager,
      getDefaultSampleRate: () => this.#audioContext?.sampleRate || 44100,
    });
  }

  async #loadWasm() {
    if (this.#cachedWasmBytes) return this.#cachedWasmBytes;

    const wasmName = this.#config.wasmUrl.split('/').pop();
    this.#eventEmitter.emit('loading:start', { type: 'wasm', name: wasmName });

    const wasmResponse = await fetch(this.#config.wasmUrl);
    if (!wasmResponse.ok) {
      throw new Error(`Failed to load WASM: ${wasmResponse.status} ${wasmResponse.statusText}`);
    }

    const wasmBytes = await wasmResponse.arrayBuffer();
    this.#eventEmitter.emit('loading:complete', { type: 'wasm', name: wasmName, size: wasmBytes.byteLength });
    this.#cachedWasmBytes = wasmBytes;

    return wasmBytes;
  }

  async #initializeAudioWorklet(wasmBytes) {
    await addWorkletModule(this.#audioContext.audioWorklet, this.#config.workletUrl);

    const numOutputChannels = this.#config.worldOptions.numOutputBusChannels;
    this.#workletNode = new AudioWorkletNode(this.#audioContext, "scsynth-processor", {
      numberOfInputs: 1,
      numberOfOutputs: 1,
      outputChannelCount: [numOutputChannels],
    });

    if (this.#config.autoConnect) {
      const dest = this.#audioContext.destination;
      if (numOutputChannels > 2) {
        dest.channelCount = Math.min(numOutputChannels, dest.maxChannelCount);
        dest.channelInterpretation = 'discrete';
      }
      this.#workletNode.connect(dest);
    }

    this.#node = this.#createNodeWrapper();
    this.#workletNode.port.start();
    this.#setupMessageHandlers();

    const mode = this.#config.mode;
    const sharedBuffer = mode === 'sab' ? this.#wasmMemory.buffer : null;

    this.#workletNode.port.postMessage({
      type: "init",
      mode: mode,
      sharedBuffer: sharedBuffer,
      snapshotIntervalMs: this.#config.snapshotIntervalMs,
    });

    const loadWasmMsg = {
      type: "loadWasm",
      wasmBytes: wasmBytes,
      worldOptions: this.#config.worldOptions,
      sampleRate: this.#audioContext.sampleRate,
    };

    if (mode === 'sab') {
      loadWasmMsg.wasmMemory = this.#wasmMemory;
    } else {
      loadWasmMsg.memoryPages = this.#config.memoryPages || 1280;
    }

    this.#workletNode.port.postMessage(loadWasmMsg);

    await this.#waitForWorkletInit();
    this.#bufferManager.setWorkletPort(this.#workletNode.port);
  }

  #createNodeWrapper() {
    const worklet = this.#workletNode;
    return Object.freeze({
      connect: (...args) => worklet.connect(...args),
      disconnect: (...args) => worklet.disconnect(...args),
      get context() { return worklet.context; },
      get numberOfOutputs() { return worklet.numberOfOutputs; },
      get numberOfInputs() { return worklet.numberOfInputs; },
      get channelCount() { return worklet.channelCount; },
      // Expose AudioWorkletNode as input for connecting external sources
      get input() { return worklet; },
    });
  }

  async #initializeOSC() {
    const mode = this.#config.mode;
    const bc = this.#metricsReader.bufferConstants;
    const ringBufferBase = this.#metricsReader.ringBufferBase;
    const sharedBuffer = this.#metricsReader.sharedBuffer;

    // Create transport based on mode
    const transportConfig = {
      workerBaseURL: this.#config.workerBaseURL,
      preschedulerCapacity: this.#config.preschedulerCapacity,
      snapshotIntervalMs: this.#config.snapshotIntervalMs,
      bypassLookaheadS: this.#config.bypassLookaheadMs / 1000,
      getAudioContextTime: () => this.#audioContext?.currentTime ?? 0,
      getNTPStartTime: () => this.#ntpTiming?.getNTPStartTime() ?? 0,
    };

    if (mode === 'sab') {
      transportConfig.sharedBuffer = sharedBuffer;
      transportConfig.ringBufferBase = ringBufferBase;
      transportConfig.bufferConstants = bc;
    }

    this.#osc = createTransport(mode, transportConfig);

    // Handle raw OSC replies - parse and dispatch
    this.#osc.onReply((oscData, sequence) => {
      // Emit raw message event
      this.#eventEmitter.emit('message:raw', { oscData, sequence });

      // Parse OSC and emit parsed message
      try {
        const msg = oscFast.decodePacket(oscData);

        // Handle special messages (msg is [address, ...args])
        const address = msg[0];
        const args = msg.slice(1);
        if (address === "/supersonic/buffer/freed") {
          this.#bufferManager?.handleBufferFreed(args);
        } else if (address === "/supersonic/buffer/allocated") {
          this.#bufferManager?.handleBufferAllocated(args);
        } else if (address === "/synced" && args.length > 0) {
          const syncId = args[0];
          if (this.#syncListeners?.has(syncId)) {
            this.#syncListeners.get(syncId)(msg);
          }
        }

        this.#eventEmitter.emit('message', msg);

        if (this.#config.debug || this.#config.debugOscIn) {
          const maxLen = this.#config.activityConsoleLog.oscInMaxLineLength ?? this.#config.activityConsoleLog.maxLineLength;
          const argsStr = args.map(a => {
            const str = JSON.stringify(a);
            return str.length > maxLen ? str.slice(0, maxLen) + '...' : str;
          }).join(', ') || '';
          console.log(`[← OSC] ${address}${argsStr ? ' ' + argsStr : ''}`);
        }
      } catch (e) {
        console.error('[SuperSonic] Failed to decode OSC message:', e);
      }
    });

    // Handle debug messages
    this.#osc.onDebug((msg) => {
      const eventMaxLen = this.#config.activityEvent.scsynthMaxLineLength ?? this.#config.activityEvent.maxLineLength;
      if (eventMaxLen > 0 && msg.text?.length > eventMaxLen) {
        msg = { ...msg, text: msg.text.slice(0, eventMaxLen) + '...' };
      }
      this.#eventEmitter.emit('debug', msg);

      if (this.#config.debug || this.#config.debugScsynth) {
        const maxLen = this.#config.activityConsoleLog.scsynthMaxLineLength ?? this.#config.activityConsoleLog.maxLineLength;
        const text = msg.text.length > maxLen ? msg.text.slice(0, maxLen) + '...' : msg.text;
        console.log(`[synth] ${text}`);
      }
    });

    // Handle errors
    this.#osc.onError((error, workerName) => {
      console.error(`[SuperSonic] ${workerName} error:`, error);
      this.#eventEmitter.emit('error', new Error(`${workerName}: ${error}`));
    });

    // Handle centralized OSC out logging (from worklet)
    this.#osc.onOscLog((entries) => {
      for (const entry of entries) {
        // Emit message:sent for each logged message, including sourceId and sequence
        this.#eventEmitter.emit('message:sent', entry.oscData, entry.sourceId, entry.sequence);
      }
    });

    // Initialize transport
    if (mode === 'sab') {
      await this.#osc.initialize();
    } else {
      await this.#osc.initialize(this.#workletNode.port);

      // PM mode: pass bufferConstants to transport for decoder worker
      this.#osc.setBufferConstants(bc);

      // Handle early debug messages that arrived before transport was ready
      if (this.#earlyDebugMessages?.length > 0) {
        for (const data of this.#earlyDebugMessages) {
          this.#osc.handleDebugRaw(data);
        }
      }
      this.#debugRawHandler = (data) => this.#osc.handleDebugRaw(data);
      this.#earlyDebugMessages = [];
    }

    // Create main-thread OscChannel for sendOSC()
    // Main thread uses sourceId 0, workers get 1+
    this.#oscChannel = this.#osc.createOscChannel({ sourceId: 0 });
  }

  async #finishInitialization() {
    this.#initialized = true;
    this.#initializing = false;
    this.bootStats.initDuration = performance.now() - this.bootStats.initStartTime;

    await this.#eventEmitter.emitAsync('setup');
    this.#eventEmitter.emit('ready', { capabilities: this.#capabilities, bootStats: this.bootStats });

    // TODO(v1): Consider whether to keep this dev console helper.
    // It auto-registers instances to window.__supersonic__ for quick debugging (ss.metrics(), ss.tree(), etc.)
    // Unusual pattern - most libs expect devs to do `window.sonic = sonic` themselves.
    // Useful but the `instances` array for multiple engines is over-engineered.
    if (__DEV__ && typeof window !== 'undefined') {
      if (!window.__supersonic__) {
        const ss = window.__supersonic__ = { instances: [] };
        Object.defineProperties(ss, {
          primary: { get: () => ss.instances[0] },
          layout: { get: () => ss.primary?.bufferConstants },
        });
        ss.metrics = () => ss.primary?.getMetrics();
        ss.tree = () => ss.primary?.getTree();
        ss.rawTree = () => ss.primary?.getRawTree();
        ss.snapshot = () => ss.primary?.getSnapshot();
      }
      window.__supersonic__.instances.push(this);
    }
  }

  #waitForWorkletInit() {
    return new Promise((resolve, reject) => {
      const timeout = setTimeout(() => {
        reject(new Error("AudioWorklet initialization timeout"));
      }, WORKLET_INIT_TIMEOUT_MS);

      const messageHandler = async (event) => {
        if (event.data.type === "error") {
          clearTimeout(timeout);
          this.#workletNode.port.removeEventListener("message", messageHandler);
          reject(new Error(event.data.error || "AudioWorklet error"));
          return;
        }

        if (event.data.type === "initialized") {
          clearTimeout(timeout);
          this.#workletNode.port.removeEventListener("message", messageHandler);

          if (event.data.success) {
            const ringBufferBase = event.data.ringBufferBase ?? 0;
            const bufferConstants = event.data.bufferConstants;
            const sharedBuffer = this.#config.mode === 'sab' ? this.#wasmMemory.buffer : null;

            // Initialize metrics reader
            this.#metricsReader.initSharedViews(sharedBuffer, ringBufferBase, bufferConstants);

            // Warn if maxNodes exceeds mirror capacity
            const maxNodes = this.#config.worldOptions?.maxNodes ?? 1024;
            const mirrorMax = bufferConstants?.NODE_TREE_MIRROR_MAX_NODES ?? 1024;
            if (maxNodes > mirrorMax) {
              console.warn(
                `SuperSonic: maxNodes (${maxNodes}) exceeds NODE_TREE_MIRROR_MAX_NODES (${mirrorMax}). ` +
                `The node tree mirror will not show all nodes. Rebuild with NODE_TREE_MIRROR_MAX_NODES=${maxNodes} to fix.`
              );
            }

            // Initialize NTP timing
            this.#ntpTiming = new NTPTiming({
              mode: this.#config.mode,
              audioContext: this.#audioContext,
              workletPort: this.#workletNode.port,
            });
            this.#ntpTiming.initSharedViews(sharedBuffer, ringBufferBase, bufferConstants);
            await this.#ntpTiming.initialize();
            this.#ntpTiming.startDriftTimer();

            // Initialize audio capture (SAB mode only)
            if (this.#config.mode === 'sab') {
              this.#audioCapture.update(sharedBuffer, ringBufferBase, bufferConstants);
            }

            // PostMessage mode: set initial snapshot
            if (this.#config.mode === 'postMessage' && event.data.initialSnapshot) {
              this.#metricsReader.updateSnapshot(event.data.initialSnapshot);
            }

            resolve();
          } else {
            reject(new Error(event.data.error || "AudioWorklet initialization failed"));
          }
        }
      };

      this.#workletNode.port.addEventListener("message", messageHandler);
      this.#workletNode.port.start();
    });
  }

  #setupMessageHandlers() {
    this.#workletNode.port.addEventListener('message', (event) => {
      const { data } = event;

      switch (data.type) {
        case "error":
          console.error("[Worklet] Error:", data.error);
          this.#eventEmitter.emit('error', new Error(data.error));
          break;

        case "version":
          this.#version = data.version;
          break;

        case "snapshot":
          if (data.buffer) {
            this.#metricsReader.updateSnapshot(data.buffer);
            this.#snapshotsSent = data.snapshotsSent;
          }
          break;

        case "debugRawBatch":
          if (this.#debugRawHandler) {
            this.#debugRawHandler(data);
          } else {
            this.#earlyDebugMessages.push(data);
          }
          break;

        case "oscLog":
          // Centralized OSC out logging from worklet (postMessage mode)
          // Forward to transport's handleOscLog which triggers the onOscLog callback
          if (data.entries && this.#osc?.handleOscLog) {
            this.#osc.handleOscLog(data.entries);
          }
          break;
      }
    });
  }

  // ============================================================================
  // PRIVATE: METRICS
  // ============================================================================

  #metricsContext() {
    return {
      preschedulerMetrics: this.#osc?.getPreschedulerMetrics(),
      transportMetrics: this.#osc?.getMetrics(),
      driftOffsetMs: this.#ntpTiming?.getDriftOffset() ?? 0,
      ntpStartTime: this.#ntpTiming?.getNTPStartTime() ?? 0,
      clockOffsetMs: this.#ntpTiming?.getClockOffset() ?? 0,
      audioContextState: this.#audioContext?.state || "unknown",
      bufferPoolStats: this.#bufferManager?.getStats(),
      loadedSynthDefsCount: this.loadedSynthDefs?.size || 0,
      preschedulerCapacity: this.#config.preschedulerCapacity,
    };
  }

  #gatherMetrics() {
    return this.#metricsReader.gatherMetrics(this.#metricsContext());
  }

  #updateMergedArray() {
    this.#metricsReader.updateMergedArray(this.#metricsContext());
  }

  // ============================================================================
  // PRIVATE: UTILITIES
  // ============================================================================

  #readProcessCount() {
    if (this.#config.mode === 'sab') {
      const view = this.#metricsReader.getMetricsView();
      return view ? view[0] : null;
    }
    const buffer = this.#metricsReader.getSnapshotBuffer();
    if (!buffer) return null;
    return new Uint32Array(buffer, 0, 1)[0];
  }

  #ensureInitialized(actionDescription = "perform this operation") {
    if (!this.#initialized) {
      throw new Error(`SuperSonic not initialized. Call init() before attempting to ${actionDescription}.`);
    }
  }

  #incrementBypassCategoryMetric(category) {
    const metricMap = {
      nonBundle: 'bypassNonBundle',
      immediate: 'bypassImmediate',
      nearFuture: 'bypassNearFuture',
      late: 'bypassLate',
    };
    const metric = metricMap[category];
    if (metric) {
      this.#metricsReader.addMetric(metric);
    }
  }

  #looksLikePathOrURL(str) {
    return str.includes("/") || str.includes("://");
  }

  #toUint8Array(data) {
    if (data instanceof Uint8Array) return data;
    if (data instanceof ArrayBuffer) return new Uint8Array(data);
    throw new Error("oscData must be ArrayBuffer or Uint8Array");
  }

  async #prepareOutboundPacket(uint8Data) {
    try {
      const decodedPacket = SuperSonic.osc.decode(uint8Data);
      const { packet, changed } = await this.#oscRewriter.rewritePacket(decodedPacket);
      if (!changed) return uint8Data;
      // Re-encode the rewritten packet
      if (packet.timeTag !== undefined) {
        return SuperSonic.osc.encodeBundle(packet.timeTag, packet.packets);
      }
      return SuperSonic.osc.encodeMessage(packet[0], packet.slice(1));
    } catch (error) {
      console.error("[SuperSonic] Failed to prepare OSC packet:", error);
      throw error;
    }
  }
}
