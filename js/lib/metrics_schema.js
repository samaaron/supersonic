// SPDX-License-Identifier: MIT OR GPL-3.0-or-later
// Copyright (c) 2025 Sam Aaron

/**
 * Canonical SuperSonic metrics schema — the single source of truth for
 * metric offsets, types, units and human-readable descriptions, shared by
 * every GUI that renders engine metrics (the <supersonic-metrics> web
 * component, Sonic Pi's Qt metrics panel, tau-state, ...).
 *
 * Offsets [0-49] index the PerformanceMetrics struct in src/shared_memory.h;
 * [50+] are context metrics merged by MetricsReader on the JS main thread.
 * Fields marked `nativeOnly: true` have no web writer (always 0 on WASM).
 *
 * The `nativeStats` section describes the separate NATIVE_STATS shm segment
 * (see NATIVE_STAT_* in shared_memory.h); `index` is the u32 slot, not a
 * PerformanceMetrics offset.
 *
 * A C++ mirror is generated from this file for native GUIs:
 *   node scripts/gen-metrics-schema-header.mjs   →   src/metrics_schema.h
 * Regenerate (and commit the header) whenever this file changes.
 */

// Descriptions for readings that combine several metrics in one row
// (e.g. "current | peak"). Referenced by the web layout below and exported
// so native GUIs can label their equivalent rows with the same words.
const COMPOSITES = {
  schedulerQueueCurrentPeak: { description: 'Current | peak scheduler queue depth' },
  schedulerLateWorstLast:    { description: 'Worst | most recent late bundle execution (ms)' },
  debugCountBytes:           { description: 'Debug messages from scsynth (count and bytes)' },
  oscSentCountBytes:         { description: 'Messages | bytes sent from host to scsynth' },
  oscRecvCountBytes:         { description: 'Messages | bytes received back from scsynth' },
  inRingUsedPeak:            { description: 'Used / peak bytes in the IN ring buffer (host to scsynth)' },
  outRingUsedPeak:           { description: 'Used / peak bytes in the OUT ring buffer (scsynth replies to host)' },
  nrtRingUsedPeak:           { description: 'Used / peak bytes in the NRT-out ring buffer (replies, notifications, debug)' },
  linkAudioChannelsRate:     { description: 'Received Link Audio channels and their sample rate' },
  linkAudioPublishSinks:     { description: 'Link Audio publishing state (1 = on) | active output sinks' },
  engineVersion:             { description: 'SuperSonic engine version' },
  busChannelsOutIn:          { description: 'Output | input audio bus channels' },
};

export const METRICS_SCHEMA = {
  metrics: {
    // scsynth metrics [0-8]
    scsynthProcessCount:          { offset: 0,  type: 'counter',  unit: 'count', description: 'Audio process() calls' },
    scsynthMessagesProcessed:     { offset: 1,  type: 'counter',  unit: 'count', description: 'OSC messages processed by scsynth' },
    scsynthMessagesDropped:       { offset: 2,  type: 'counter',  unit: 'count', description: 'Messages dropped (ring buffer full)' },
    scsynthSchedulerDepth:        { offset: 3,  type: 'gauge',    unit: 'count', description: 'Current scheduler queue depth' },
    scsynthSchedulerPeakDepth:    { offset: 4,  type: 'gauge',    unit: 'count', description: 'Peak scheduler queue depth (high water mark)' },
    scsynthSchedulerDropped:      { offset: 5,  type: 'counter',  unit: 'count', description: 'Events dropped because the scheduler queue overflowed' },
    scsynthSequenceGaps:          { offset: 6,  type: 'counter',  unit: 'count', description: 'Messages lost in transit from host to scsynth' },
    scsynthWasmErrors:            { offset: 7,  type: 'counter',  unit: 'count', description: 'WASM execution errors in audio worklet' },
    scsynthSchedulerLates:        { offset: 8,  type: 'counter',  unit: 'count', description: 'Bundles executed after their scheduled time' },

    // OSC Out metrics [9-10]
    oscOutMessagesSent:           { offset: 9,  type: 'counter',  unit: 'count', description: 'OSC messages sent from host to scsynth' },
    oscOutBytesSent:              { offset: 10, type: 'counter',  unit: 'bytes', description: 'Total bytes sent from host to scsynth' },

    // OSC In metrics [11-14]
    oscInMessagesReceived:        { offset: 11, type: 'counter',  unit: 'count', description: 'OSC replies received from scsynth' },
    oscInBytesReceived:           { offset: 12, type: 'counter',  unit: 'bytes', description: 'Total bytes received from scsynth' },
    oscInMessagesDropped:         { offset: 13, type: 'counter',  unit: 'count', description: 'Replies lost in transit from scsynth to host' },
    oscInCorrupted:               { offset: 14, type: 'counter',  unit: 'count', description: 'Corrupted messages detected in the ring buffer' },

    // Debug metrics [15-16]
    debugMessagesReceived:        { offset: 15, type: 'counter',  unit: 'count', description: 'Debug messages from scsynth' },
    debugBytesReceived:           { offset: 16, type: 'counter',  unit: 'bytes', description: 'Debug bytes received' },

    // Ring buffer usage [17-22]
    inBufferUsedBytes:            { offset: 17, type: 'gauge',    unit: 'bytes', description: 'Bytes used in IN ring buffer' },
    outBufferUsedBytes:           { offset: 18, type: 'gauge',    unit: 'bytes', description: 'Bytes used in OUT ring buffer' },
    nrtOutBufferUsedBytes:         { offset: 19, type: 'gauge',    unit: 'bytes', description: 'Bytes used in NRT-out ring buffer' },
    inBufferPeakBytes:            { offset: 20, type: 'gauge',    unit: 'bytes', description: 'Peak bytes used in IN ring buffer' },
    outBufferPeakBytes:           { offset: 21, type: 'gauge',    unit: 'bytes', description: 'Peak bytes used in OUT ring buffer' },
    nrtOutBufferPeakBytes:         { offset: 22, type: 'gauge',    unit: 'bytes', description: 'Peak bytes used in NRT-out ring buffer' },

    // scsynth late timing diagnostics [23-25]
    scsynthSchedulerMaxLateMs:    { offset: 23, type: 'gauge',    unit: 'ms',    description: 'Maximum lateness observed in scsynth scheduler (ms)' },
    scsynthSchedulerLastLateMs:   { offset: 24, type: 'gauge',    unit: 'ms',    description: 'Most recent late magnitude in scsynth scheduler (ms)' },
    scsynthSchedulerLastLateTick: { offset: 25, type: 'gauge',    unit: 'count', description: 'Process count when last scsynth late occurred' },

    // Ring buffer direct write failures [26]
    ringBufferDirectWriteFails:   { offset: 26, type: 'counter',  unit: 'count', description: 'SAB mode only: direct IN-ring writes that lost the lock race or hit a full ring and were dropped (no fallback)' },

    // Link session [27-31] — native-only (no web writer; always 0 on WASM).
    // SuperClock session state lives in its own SAB region (SuperClockState),
    // not in PerformanceMetrics. Owned by engine.superClock.
    linkPeers:                    { offset: 27, type: 'gauge',    unit: 'count',    nativeOnly: true, description: 'Connected Ableton Link peers on the network' },
    linkTempoMbpm:                { offset: 28, type: 'gauge',    unit: 'milliBpm', nativeOnly: true, description: 'Shared Link session tempo' },
    linkBeatCenti:                { offset: 29, type: 'gauge',    unit: 'centi',    nativeOnly: true, description: 'Current Link beat position' },
    linkPhaseCenti:               { offset: 30, type: 'gauge',    unit: 'centi',    nativeOnly: true, description: 'Phase within the Link quantum' },
    linkPlaying:                  { offset: 31, type: 'gauge',    unit: 'bool',     nativeOnly: true, description: 'Link transport playing (0/1)' },

    // Link Audio stream health [32-38] — native-only (no web writer)
    linkAudioInChannels:          { offset: 32, type: 'gauge',    unit: 'count', nativeOnly: true, description: 'Active received Link Audio channels' },
    linkAudioStreamRate:          { offset: 33, type: 'gauge',    unit: 'Hz',    nativeOnly: true, description: 'Received Link Audio stream sample rate' },
    linkAudioUnderruns:           { offset: 34, type: 'counter',  unit: 'count', nativeOnly: true, description: 'Receiver queue underruns (stream audio arrived too late to play)' },
    linkAudioBufferedMs:          { offset: 35, type: 'gauge',    unit: 'ms',    nativeOnly: true, description: 'Received Link Audio queued in the receiver (ms)' },
    linkAudioDriftPpm:            { offset: 36, type: 'gauge',    unit: 'ppm',   signed: true, nativeOnly: true, description: "Read-rate deviation from the sender's clock (parts per million)" },
    linkAudioPublish:             { offset: 37, type: 'gauge',    unit: 'bool',  nativeOnly: true, description: 'Link Audio publishing enabled (0/1)' },
    linkAudioSinks:               { offset: 38, type: 'gauge',    unit: 'count', nativeOnly: true, description: 'Active Link Audio output sinks' },

    // System info [39-45] — cross-platform; written by shared C++ at init.
    supersonicVersionMajor:       { offset: 39, type: 'constant', unit: 'count', description: 'SuperSonic major version' },
    supersonicVersionMinor:       { offset: 40, type: 'constant', unit: 'count', description: 'SuperSonic minor version' },
    supersonicVersionPatch:       { offset: 41, type: 'constant', unit: 'count', description: 'SuperSonic patch version' },
    audioSampleRate:              { offset: 42, type: 'constant', unit: 'Hz',    description: 'Output sample rate' },
    audioBlockSize:               { offset: 43, type: 'constant', unit: 'count', description: 'Audio block size in frames per callback' },
    audioOutputChannels:          { offset: 44, type: 'constant', unit: 'count', description: 'Output bus channels' },
    audioInputChannels:           { offset: 45, type: 'constant', unit: 'count', description: 'Input bus channels' },

    // SuperClock readouts [46-49] — cross-platform; written per block.
    clockTempoMbpm:               { offset: 46, type: 'gauge',    unit: 'milliBpm', description: "Tempo of the engine's internal SuperClock" },
    clockBeatCenti:               { offset: 47, type: 'gauge',    unit: 'centi',    description: 'Current SuperClock beat position' },
    clockPhaseCenti:              { offset: 48, type: 'gauge',    unit: 'centi',    description: 'Phase within the quantum' },
    clockPlaying:                 { offset: 49, type: 'gauge',    unit: 'bool',     description: 'Transport playing (0/1)' },

    // Context metrics [50+] (main thread only)
    driftOffsetMs:                { offset: 50, type: 'gauge',    unit: 'ms',    signed: true, description: 'Clock drift between AudioContext and wall clock' },
    clockOffsetMs:                { offset: 51, type: 'gauge',    unit: 'ms',    signed: true, description: 'Clock offset for multi-system sync' },
    audioContextState:            { offset: 52, type: 'enum',     values: ['unknown', 'running', 'suspended', 'closed', 'interrupted'], description: 'AudioContext state' },
    bufferPoolUsedBytes:          { offset: 53, type: 'gauge',    unit: 'bytes', description: 'Buffer pool bytes used' },
    bufferPoolAvailableBytes:     { offset: 54, type: 'gauge',    unit: 'bytes', description: 'Buffer pool bytes available' },
    bufferPoolAllocations:        { offset: 55, type: 'counter',  unit: 'count', description: 'Total buffer allocations' },
    loadedSynthDefs:              { offset: 56, type: 'gauge',    unit: 'count', description: 'Number of loaded synthdefs' },
    scsynthSchedulerCapacity:     { offset: 57, type: 'constant', unit: 'count', description: 'Maximum scheduler queue size' },
    inBufferCapacity:             { offset: 58, type: 'constant', unit: 'bytes', description: 'IN ring buffer capacity' },
    outBufferCapacity:            { offset: 59, type: 'constant', unit: 'bytes', description: 'OUT ring buffer capacity' },
    nrtOutBufferCapacity:          { offset: 60, type: 'constant', unit: 'bytes', description: 'NRT-out ring buffer capacity' },
    mode:                         { offset: 61, type: 'enum',     values: ['sab', 'postMessage'], description: 'Transport mode' },

    // Audio diagnostics [62-68] (main thread, Chrome playbackStats + cross-browser health)
    glitchCount:                  { offset: 62, type: 'counter',  unit: 'count', description: 'Chrome only: audio underrun/glitch events' },
    glitchDurationMs:             { offset: 63, type: 'gauge',    unit: 'ms',    description: 'Chrome only: total silence from audio underruns' },
    averageLatencyUs:             { offset: 64, type: 'gauge',    unit: 'us',    description: 'Chrome only: average audio output latency' },
    maxLatencyUs:                 { offset: 65, type: 'gauge',    unit: 'us',    description: 'Chrome only: maximum audio output latency' },
    audioHealthPct:               { offset: 66, type: 'gauge',    unit: '%',     description: 'Cross-browser: fraction of expected audio frames delivered (100% = no issues)' },
    totalFramesDurationMs:        { offset: 67, type: 'counter',  unit: 'ms',    description: 'Chrome only: total audio rendered duration' },
    hasPlaybackStats:             { offset: 68, type: 'gauge',    unit: 'bool',  description: '1 if Chrome playbackStats API is available, 0 otherwise' },

    // Buffer pool growth metrics [69-72] (main thread)
    bufferPoolTotalCapacity:      { offset: 69, type: 'gauge',    unit: 'bytes', description: 'Buffer pool committed capacity (grows on demand)' },
    bufferPoolMaxCapacity:        { offset: 70, type: 'gauge',    unit: 'bytes', description: 'Buffer pool hard ceiling' },
    bufferPoolGrowthCount:        { offset: 71, type: 'counter',  unit: 'count', description: 'Number of buffer pool growth events' },
    bufferPoolPoolCount:          { offset: 72, type: 'gauge',    unit: 'count', description: 'Number of buffer pool segments' },
  },

  // NATIVE_STATS shm segment (native/JUCE backend only; see NATIVE_STAT_* in
  // src/shared_memory.h). `index` is the u32 slot within the segment — a
  // separate address space from the PerformanceMetrics offsets above.
  nativeStats: {
    synthDefs:    { index: 0, type: 'gauge',   unit: 'count', description: 'Synth definitions currently loaded in the engine' },
    buffers:      { index: 1, type: 'gauge',   unit: 'count', description: 'Allocated sample buffers' },
    bufferBytes:  { index: 2, type: 'gauge',   unit: 'bytes', description: 'Total memory held by sample buffers' },
    cpuAvgCenti:  { index: 3, type: 'gauge',   unit: 'centi', description: 'Average DSP load as a share of the audio callback time budget (% * 100)' },
    cpuPeakCenti: { index: 4, type: 'gauge',   unit: 'centi', description: 'Decaying peak DSP load (% * 100). Sustained values near 100% risk audible glitches' },
    cbOverruns:   { index: 5, type: 'counter', unit: 'count', description: 'Audio callbacks that overran their time budget' },
  },

  composites: COMPOSITES,

  layout: {
    panels: [
      {
        title: 'OSC Out',
        rows: [
          { label: 'sent',   cells: [{ key: 'oscOutMessagesSent' }] },
          { label: 'bytes',  cells: [{ key: 'oscOutBytesSent', kind: 'muted', format: 'bytes' }] },
          { label: 'lost',   cells: [{ key: 'scsynthSequenceGaps', kind: 'error' }] },
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
        title: 'scsynth Scheduler',
        rows: [
          { label: 'queue',   tooltip: COMPOSITES.schedulerQueueCurrentPeak.description, cells: [{ key: 'scsynthSchedulerDepth' }, { sep: ' | ' }, { key: 'scsynthSchedulerPeakDepth', kind: 'muted' }] },
          { label: 'dropped', cells: [{ key: 'scsynthSchedulerDropped', kind: 'error' }] },
          { label: 'lates',   cells: [{ key: 'scsynthSchedulerLates', kind: 'error' }] },
          { label: 'max | last', tooltip: COMPOSITES.schedulerLateWorstLast.description, cells: [{ key: 'scsynthSchedulerMaxLateMs', kind: 'error' }, { sep: ' | ' }, { key: 'scsynthSchedulerLastLateMs', kind: 'dim' }, { text: ' ms', kind: 'muted' }] },
        ]
      },
      {
        title: 'scsynth',
        rows: [
          { label: 'ticks',       tooltip: 'Audio process() callback count and OSC messages processed', cells: [{ key: 'scsynthProcessCount', kind: 'dim' }, { sep: ' | ' }, { key: 'scsynthMessagesProcessed', kind: 'muted' }, { text: ' msgs', kind: 'muted' }] },
          { label: 'dropped',     cells: [{ key: 'scsynthMessagesDropped', kind: 'error' }] },
          { label: 'drift',       cells: [{ key: 'driftOffsetMs', format: 'signed' }, { text: ' ms', kind: 'muted' }] },
          { label: 'debug',       tooltip: COMPOSITES.debugCountBytes.description, cells: [{ key: 'debugMessagesReceived', kind: 'muted' }, { text: ' (' }, { key: 'debugBytesReceived', kind: 'muted', format: 'bytes' }, { text: ')' }] },
        ]
      },
      {
        title: 'Ring Buffer Level',
        class: 'wide',
        rows: [
          { type: 'bar', label: 'in',  usedKey: 'inBufferUsedBytes',  peakKey: 'inBufferPeakBytes',  capacityKey: 'inBufferCapacity',  color: 'blue' },
          { type: 'bar', label: 'out', usedKey: 'outBufferUsedBytes', peakKey: 'outBufferPeakBytes', capacityKey: 'outBufferCapacity', color: 'green' },
          { type: 'bar', label: 'dbg', usedKey: 'nrtOutBufferUsedBytes', peakKey: 'nrtOutBufferPeakBytes', capacityKey: 'nrtOutBufferCapacity', color: 'purple' },
          { label: 'direct write fails', cells: [{ key: 'ringBufferDirectWriteFails', kind: 'error' }] },
        ]
      },
      {
        title: 'Buffers & SynthDefs',
        rows: [
          { label: 'buf used',  cells: [{ key: 'bufferPoolUsedBytes', format: 'bytes' }] },
          { label: 'buf free',  cells: [{ key: 'bufferPoolAvailableBytes', kind: 'green', format: 'bytes' }] },
          { label: 'buf allocs', cells: [{ key: 'bufferPoolAllocations', kind: 'dim' }] },
          { label: 'synthdefs', cells: [{ key: 'loadedSynthDefs' }] },
        ]
      },
      {
        title: 'AudioWorklet',
        rows: [
          { label: 'health',      tooltip: 'AudioContext state and audio health percentage (fraction of expected frames delivered)', cells: [{ key: 'audioContextState', kind: 'green', format: 'enum' }, { sep: ' | ' }, { key: 'audioHealthPct', kind: 'green', format: 'percent' }, { text: ' %', kind: 'muted' }] },
          { label: 'glitches',    tooltip: 'Chrome only: audio underrun/glitch events and total silence duration', cells: [{ key: 'glitchCount', kind: 'error', format: 'chromeOnly' }, { sep: ' (' }, { key: 'glitchDurationMs', kind: 'error', format: 'chromeOnly' }, { text: ' ms)', kind: 'muted' }] },
          { label: 'latency',     tooltip: 'Chrome only: avg | max audio output latency in ms', cells: [{ key: 'averageLatencyUs', kind: 'dim', format: 'chromeLatencyUs' }, { sep: ' | ' }, { key: 'maxLatencyUs', kind: 'dim', format: 'chromeLatencyUs' }, { text: ' ms', kind: 'muted' }] },
          { label: 'WASM errors', cells: [{ key: 'scsynthWasmErrors', kind: 'error' }] },
        ]
      },
      {
        title: 'Engine',
        rows: [
          { label: 'version',  cells: [{ key: 'supersonicVersionMajor' }, { text: '.' }, { key: 'supersonicVersionMinor' }, { text: '.' }, { key: 'supersonicVersionPatch' }] },
          { label: 'rate',     cells: [{ key: 'audioSampleRate' }, { text: ' Hz', kind: 'muted' }] },
          { label: 'block',    cells: [{ key: 'audioBlockSize' }, { text: ' frames', kind: 'muted' }] },
          { label: 'channels', tooltip: COMPOSITES.busChannelsOutIn.description, cells: [{ key: 'audioOutputChannels' }, { sep: ' | ' }, { key: 'audioInputChannels', kind: 'muted' }] },
        ]
      },
      {
        title: 'Clock',
        rows: [
          { label: 'tempo',   cells: [{ key: 'clockTempoMbpm', format: 'milliBpm' }, { text: ' bpm', kind: 'muted' }] },
          { label: 'beat',    cells: [{ key: 'clockBeatCenti', kind: 'dim', format: 'centi' }] },
          { label: 'phase',   cells: [{ key: 'clockPhaseCenti', kind: 'dim', format: 'centi' }] },
          { label: 'playing', cells: [{ key: 'clockPlaying', kind: 'muted' }] },
        ]
      },
    ]
  },
};
