// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2025-2026 Sam Aaron
/*
 * scsynth_profile.js — scsynth's vocabulary, declared to clockwork.
 *
 * Clockwork carries no DSP and no definition format, so it cannot know that
 * a definition is called a synthdef, that it arrives on /d_recv, that its name
 * is a length-prefixed string eleven bytes in, or that a barrier is /sync
 * answered by /synced. Those are scsynth's, and this is where SuperSonic says
 * so — the JavaScript counterpart of dsp_describe() on the C side.
 *
 * Without it the client falls back to NO_DSP: definitions are never cached, so
 * a device switch restores nothing, and sync() refuses because no verb asks.
 */

import { extractSynthDefName } from "./lib/synthdef_parser.js";

/*
 * Name extraction is `synthdef_parser.js`, not a reimplementation here.
 *
 * This file carried its own thirteen-line SCgf reader until 2026-08-31. It
 * looked right and got v3 wrong, and it also could not take a path or URL —
 * so every synthdef_versions case asking for a name got `undefined` back.
 * The real parser was already sitting in js/lib, brought across with the rest
 * of the product code and never wired up.
 */
export const scsynthProfile = Object.freeze({
  defineVerb: "/d_recv",
  nameOf: extractSynthDefName,
  forgetVerb: "/d_free",
  forgetAllVerb: "/d_freeAll",
  syncVerb: "/sync",
  syncedVerb: "/synced",
  /*
   * Verbs the engine would take literally, refused client-side.
   *
   * Each names a file or a scheduler the browser does not have, or a setting
   * this product deliberately fixes. Refusing them here, with the alternative
   * in the message, is better than letting them reach an engine that will
   * either fail obscurely or quietly do nothing.
   */
  blockedVerbs: Object.freeze({
    "/d_load":        "Use loadSynthDef() or send /d_recv with synthdef bytes instead.",
    "/d_loadDir":     "Use loadSynthDef() or send /d_recv with synthdef bytes instead.",
    "/b_read":        "Use loadSample() to load audio into a buffer.",
    "/b_readChannel": "Use loadSample() to load audio into a buffer.",
    "/b_write":       "Writing audio files is not available in the browser.",
    "/b_close":       "Writing audio files is not available in the browser.",
    "/clearSched":    "Use purge() to clear both the JS prescheduler and WASM scheduler.",
    "/error":         "SuperSonic always enables error notifications so you never miss a /fail message.",
  }),

  /*
   * What scsynth exposes that clockwork cannot name.
   *
   * Its sample buffer pool: how much is in it, how much is left, how often it
   * has had to grow. These sat at fixed offsets in clockwork's own metrics
   * files until 2026-08-31, which meant a guest with different numbers — and
   * clockwork-vm's are entirely different — could not report them at all.
   *
   * `slot` is an index into the reserved guest range, not an absolute offset.
   */
  metrics: {
    bufferPoolUsedBytes:      { slot: 0, type: "u32", unit: "bytes",
                                description: "sample buffer pool bytes in use" },
    bufferPoolAvailableBytes: { slot: 1, type: "u32", unit: "bytes",
                                description: "sample buffer pool bytes free" },
    bufferPoolAllocations:    { slot: 2, type: "u32",
                                description: "buffers currently allocated" },
    bufferPoolTotalCapacity:  { slot: 3, type: "u32", unit: "bytes",
                                description: "committed capacity across all pool segments" },
    bufferPoolMaxCapacity:    { slot: 4, type: "u32", unit: "bytes",
                                description: "hard ceiling the pool may grow to" },
    bufferPoolGrowthCount:    { slot: 5, type: "u32",
                                description: "times the pool has grown" },
    bufferPoolPoolCount:      { slot: 6, type: "u32",
                                description: "pool segments; 1 means it has never grown" },
    loadedSynthDefs:          { slot: 7, type: "u32",
                                description: "definitions this client holds, and will replay after a reload" },
  },

  /*
   * Where those numbers appear in the metrics UI.
   *
   * clockwork's static layout carried this panel until 2026-08-31, which
   * meant a shared layout naming one guest's counters — and a guest with no
   * sample pool got four rows that could only ever read zero. A panel is a
   * claim about what a guest HAS, so it travels with the declaration.
   */
  metricsPanels: [
    {
      title: "Buffers & SynthDefs",
      rows: [
        { label: "buf used",   cells: [{ key: "bufferPoolUsedBytes", format: "bytes" }] },
        { label: "buf free",   cells: [{ key: "bufferPoolAvailableBytes", kind: "green", format: "bytes" }] },
        { label: "buf allocs", cells: [{ key: "bufferPoolAllocations", kind: "dim" }] },
        { label: "synthdefs",  cells: [{ key: "loadedSynthDefs" }] },
      ],
    },
  ],
});
