// SPDX-License-Identifier: MIT OR GPL-3.0-or-later
// Copyright (c) 2025 Sam Aaron
//
// GENERATED FILE — DO NOT EDIT.
// Source of truth: js/lib/metrics_schema.js
// Regenerate with:  npm run gen:metrics-header
// Staleness is enforced by test/unit/metrics_schema_header.test.mjs.
//
// Metric names, units and human-readable descriptions for SuperSonic's
// performance metrics, for use by native GUIs. Offsets index the
// PerformanceMetrics struct (see shared_memory.h); native-stat indices
// address the separate NATIVE_STATS segment.

#pragma once

#include <cstdint>
#include <cstring>

namespace supersonic {
namespace metrics_schema {

struct FieldInfo
{
    uint32_t offset;         // index into the metrics Uint32 array
    const char* key;         // schema key (stable identifier)
    const char* unit;        // unit of measurement ("" if none)
    const char* description; // human-readable description
};

inline constexpr FieldInfo kFields[] = {
    {  0, "scsynthProcessCount", "count", "Audio process() calls" },
    {  1, "scsynthMessagesProcessed", "count", "OSC messages processed by scsynth" },
    {  2, "scsynthMessagesDropped", "count", "Messages dropped (ring buffer full)" },
    {  3, "scsynthSchedulerDepth", "count", "Current scheduler queue depth" },
    {  4, "scsynthSchedulerPeakDepth", "count", "Peak scheduler queue depth (high water mark)" },
    {  5, "scsynthSchedulerDropped", "count", "Events dropped because the scheduler queue overflowed" },
    {  6, "scsynthSequenceGaps", "count", "Messages lost in transit from host to scsynth" },
    {  7, "scsynthWasmErrors", "count", "WASM execution errors in audio worklet" },
    {  8, "scsynthSchedulerLates", "count", "Bundles executed after their scheduled time" },
    {  9, "oscOutMessagesSent", "count", "OSC messages sent from host to scsynth" },
    { 10, "oscOutBytesSent", "bytes", "Total bytes sent from host to scsynth" },
    { 11, "oscInMessagesReceived", "count", "OSC replies received from scsynth" },
    { 12, "oscInBytesReceived", "bytes", "Total bytes received from scsynth" },
    { 13, "oscInMessagesDropped", "count", "Replies lost in transit from scsynth to host" },
    { 14, "oscInCorrupted", "count", "Corrupted messages detected in the ring buffer" },
    { 15, "debugMessagesReceived", "count", "Debug messages from scsynth" },
    { 16, "debugBytesReceived", "bytes", "Debug bytes received" },
    { 17, "inBufferUsedBytes", "bytes", "Bytes used in IN ring buffer" },
    { 18, "outBufferUsedBytes", "bytes", "Bytes used in OUT ring buffer" },
    { 19, "nrtOutBufferUsedBytes", "bytes", "Bytes used in NRT-out ring buffer" },
    { 20, "inBufferPeakBytes", "bytes", "Peak bytes used in IN ring buffer" },
    { 21, "outBufferPeakBytes", "bytes", "Peak bytes used in OUT ring buffer" },
    { 22, "nrtOutBufferPeakBytes", "bytes", "Peak bytes used in NRT-out ring buffer" },
    { 23, "scsynthSchedulerMaxLateMs", "ms", "Maximum lateness observed in scsynth scheduler (ms)" },
    { 24, "scsynthSchedulerLastLateMs", "ms", "Most recent late magnitude in scsynth scheduler (ms)" },
    { 25, "scsynthSchedulerLastLateTick", "count", "Process count when last scsynth late occurred" },
    { 26, "ringBufferDirectWriteFails", "count", "SAB mode only: direct IN-ring writes that lost the lock race or hit a full ring and were dropped (no fallback)" },
    { 27, "linkPeers", "count", "Connected Ableton Link peers on the network" },
    { 28, "linkTempoMbpm", "milliBpm", "Shared Link session tempo" },
    { 29, "linkBeatCenti", "centi", "Current Link beat position" },
    { 30, "linkPhaseCenti", "centi", "Phase within the Link quantum" },
    { 31, "linkPlaying", "bool", "Link transport playing (0/1)" },
    { 32, "linkAudioInChannels", "count", "Active received Link Audio channels" },
    { 33, "linkAudioStreamRate", "Hz", "Received Link Audio stream sample rate" },
    { 34, "linkAudioUnderruns", "count", "Receiver queue underruns (stream audio arrived too late to play)" },
    { 35, "linkAudioBufferedMs", "ms", "Received Link Audio queued in the receiver (ms)" },
    { 36, "linkAudioDriftPpm", "ppm", "Read-rate deviation from the sender's clock (parts per million)" },
    { 37, "linkAudioPublish", "bool", "Link Audio publishing enabled (0/1)" },
    { 38, "linkAudioSinks", "count", "Active Link Audio output sinks" },
    { 39, "supersonicVersionMajor", "count", "SuperSonic major version" },
    { 40, "supersonicVersionMinor", "count", "SuperSonic minor version" },
    { 41, "supersonicVersionPatch", "count", "SuperSonic patch version" },
    { 42, "audioSampleRate", "Hz", "Output sample rate" },
    { 43, "audioBlockSize", "count", "Audio block size in frames per callback" },
    { 44, "audioOutputChannels", "count", "Output bus channels" },
    { 45, "audioInputChannels", "count", "Input bus channels" },
    { 46, "clockTempoMbpm", "milliBpm", "Tempo of the engine's internal SuperClock" },
    { 47, "clockBeatCenti", "centi", "Current SuperClock beat position" },
    { 48, "clockPhaseCenti", "centi", "Phase within the quantum" },
    { 49, "clockPlaying", "bool", "Transport playing (0/1)" },
    { 50, "driftOffsetMs", "ms", "Clock drift between AudioContext and wall clock" },
    { 51, "clockOffsetMs", "ms", "Clock offset for multi-system sync" },
    { 52, "audioContextState", "", "AudioContext state" },
    { 53, "bufferPoolUsedBytes", "bytes", "Buffer pool bytes used" },
    { 54, "bufferPoolAvailableBytes", "bytes", "Buffer pool bytes available" },
    { 55, "bufferPoolAllocations", "count", "Total buffer allocations" },
    { 56, "loadedSynthDefs", "count", "Number of loaded synthdefs" },
    { 57, "scsynthSchedulerCapacity", "count", "Maximum scheduler queue size" },
    { 58, "inBufferCapacity", "bytes", "IN ring buffer capacity" },
    { 59, "outBufferCapacity", "bytes", "OUT ring buffer capacity" },
    { 60, "nrtOutBufferCapacity", "bytes", "NRT-out ring buffer capacity" },
    { 61, "mode", "", "Transport mode" },
    { 62, "glitchCount", "count", "Chrome only: audio underrun/glitch events" },
    { 63, "glitchDurationMs", "ms", "Chrome only: total silence from audio underruns" },
    { 64, "averageLatencyUs", "us", "Chrome only: average audio output latency" },
    { 65, "maxLatencyUs", "us", "Chrome only: maximum audio output latency" },
    { 66, "audioHealthPct", "%", "Cross-browser: fraction of expected audio frames delivered (100% = no issues)" },
    { 67, "totalFramesDurationMs", "ms", "Chrome only: total audio rendered duration" },
    { 68, "hasPlaybackStats", "bool", "1 if Chrome playbackStats API is available, 0 otherwise" },
    { 69, "bufferPoolTotalCapacity", "bytes", "Buffer pool committed capacity (grows on demand)" },
    { 70, "bufferPoolMaxCapacity", "bytes", "Buffer pool hard ceiling" },
    { 71, "bufferPoolGrowthCount", "count", "Number of buffer pool growth events" },
    { 72, "bufferPoolPoolCount", "count", "Number of buffer pool segments" },
};

struct NativeStatInfo
{
    uint32_t index;          // u32 slot within the NATIVE_STATS segment
    const char* key;
    const char* unit;
    const char* description;
};

inline constexpr NativeStatInfo kNativeStats[] = {
    { 0, "synthDefs", "count", "Synth definitions currently loaded in the engine" },
    { 1, "buffers", "count", "Allocated sample buffers" },
    { 2, "bufferBytes", "bytes", "Total memory held by sample buffers" },
    { 3, "cpuAvgCenti", "centi", "Average DSP load as a share of the audio callback time budget (% * 100)" },
    { 4, "cpuPeakCenti", "centi", "Decaying peak DSP load (% * 100). Sustained values near 100% risk audible glitches" },
    { 5, "cbOverruns", "count", "Audio callbacks that overran their time budget" },
    { 6, "nrtMaxPassMs", "ms", "Longest the control thread has spent handling one batch of commands since boot" },
    { 7, "nrtInFlightMs", "ms", "How long the control thread has been stuck in the command it is handling right now. Anything but 0 means later commands, and every reply behind them, are waiting" },
};

// Rows combining several metrics in one reading ("current | peak", ...).
struct CompositeInfo
{
    const char* key;
    const char* description;
};

inline constexpr CompositeInfo kComposites[] = {
    { "schedulerQueueCurrentPeak", "Current | peak scheduler queue depth" },
    { "schedulerLateWorstLast", "Worst | most recent late bundle execution (ms)" },
    { "debugCountBytes", "Debug messages from scsynth (count and bytes)" },
    { "oscSentCountBytes", "Messages | bytes sent from host to scsynth" },
    { "oscRecvCountBytes", "Messages | bytes received back from scsynth" },
    { "inRingUsedPeak", "Used / peak bytes in the IN ring buffer (host to scsynth)" },
    { "outRingUsedPeak", "Used / peak bytes in the OUT ring buffer (scsynth replies to host)" },
    { "nrtRingUsedPeak", "Used / peak bytes in the NRT-out ring buffer (replies, notifications, debug)" },
    { "linkAudioChannelsRate", "Received Link Audio channels and their sample rate" },
    { "linkAudioPublishSinks", "Link Audio publishing state (1 = on) | active output sinks" },
    { "engineVersion", "SuperSonic engine version" },
    { "busChannelsOutIn", "Output | input audio bus channels" },
};

inline const char* descriptionForComposite(const char* key)
{
    for (const CompositeInfo& c : kComposites)
        if (std::strcmp(c.key, key) == 0)
            return c.description;
    return nullptr;
}

inline const char* descriptionForOffset(uint32_t offset)
{
    for (const FieldInfo& f : kFields)
        if (f.offset == offset)
            return f.description;
    return nullptr;
}

inline const char* descriptionForNativeStat(uint32_t index)
{
    for (const NativeStatInfo& f : kNativeStats)
        if (f.index == index)
            return f.description;
    return nullptr;
}

} // namespace metrics_schema
} // namespace supersonic
