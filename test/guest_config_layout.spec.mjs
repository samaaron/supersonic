/**
 * guest_config_layout.spec.mjs — Buffer layout tests for GUEST_CONFIG_START
 *
 * Validates that:
 *   - GUEST_CONFIG_START is exposed in bufferConstants
 *   - GUEST_CONFIG_START is outside the IN ring buffer
 *   - GUEST_CONFIG_SIZE is a ceiling the guest's block fits inside
 *
 * The size was pinned at 72 bytes here — scsynth's eighteen WorldOptions
 * slots, checked as though clockwork knew what the block meant. It does not:
 * shared_memory.h calls the region "a ceiling, not a shape", a host and its
 * guest agree the layout privately, and this repository has two hosts writing
 * two different ones. So the assertion is what clockwork actually promises —
 * room for the block, whole 32-bit slots, and a region that opens the guest's
 * own run, after everything the guest publishes.
 */
import { test, expect } from "./fixtures.mjs";

test("bufferConstants includes GUEST_CONFIG_START and GUEST_CONFIG_SIZE", async ({ sonicPage, sonicConfig }) => {
  const result = await sonicPage.evaluate(async (config) => {
    const sonic = new window.SuperSonic(config);
    await sonic.init();

    const bc = sonic.bufferConstants;
    await sonic.shutdown();

    return {
      hasStart: typeof bc.GUEST_CONFIG_START === "number",
      hasSize: typeof bc.GUEST_CONFIG_SIZE === "number",
      start: bc.GUEST_CONFIG_START,
      size: bc.GUEST_CONFIG_SIZE,
    };
  }, sonicConfig);

  expect(result.hasStart).toBe(true);
  expect(result.hasSize).toBe(true);
  expect(result.start).toBeGreaterThan(0);

  // A whole number of u32 slots, and big enough for the largest block either
  // host writes: the web encoder's eighteen and the native host's nine.
  expect(result.size % 4).toBe(0);
  expect(result.size).toBeGreaterThanOrEqual(18 * 4);
});

test("GUEST_CONFIG_START is outside IN ring buffer", async ({ sonicPage, sonicConfig }) => {
  const result = await sonicPage.evaluate(async (config) => {
    const sonic = new window.SuperSonic(config);
    await sonic.init();

    const bc = sonic.bufferConstants;
    await sonic.shutdown();

    return {
      inBufferStart: bc.IN_RING_START,
      inBufferSize: bc.IN_RING_SIZE,
      worldOptionsStart: bc.GUEST_CONFIG_START,
      totalBufferSize: bc.ARENA_SIZE,
    };
  }, sonicConfig);

  // GUEST_CONFIG_START must be beyond the IN ring buffer
  expect(result.worldOptionsStart).toBeGreaterThanOrEqual(
    result.inBufferStart + result.inBufferSize
  );
  // And must fit within total buffer
  expect(result.worldOptionsStart + 72).toBeLessThanOrEqual(result.totalBufferSize);
});

test("GUEST_CONFIG_START is consistent with ARENA_SIZE", async ({ sonicPage, sonicConfig }) => {
  const result = await sonicPage.evaluate(async (config) => {
    const sonic = new window.SuperSonic(config);
    await sonic.init();

    const bc = sonic.bufferConstants;
    await sonic.shutdown();

    return {
      worldOptionsStart: bc.GUEST_CONFIG_START,
      worldOptionsSize: bc.GUEST_CONFIG_SIZE,
      windowStart: bc.GUEST_WINDOW_START,
      windowSize: bc.GUEST_WINDOW_SIZE,
      scopeStart: bc.TAPS_START,          // the taps: in clockwork's half, after the scope
      scopeTotalSize: bc.TAPS_SIZE,
      sampleClockStart: bc.SAMPLE_CLOCK_START,
      sampleClockSize: bc.SAMPLE_CLOCK_SIZE,
      guestPersistStart: bc.GUEST_PERSIST_START,
      guestPersistSize: bc.GUEST_PERSIST_SIZE,
      channelMapStart: bc.CHANNEL_MAP_START,
      channelMapSize: bc.CHANNEL_MAP_SIZE,
      nrtOutStart: bc.NRT_OUT_RING_START,
      nrtOutSize: bc.NRT_OUT_RING_SIZE,
      clockworkBlockSize: bc.CLOCKWORK_HALF_SIZE,
      guestRegionStart: bc.GUEST_HALF_START,
      totalBufferSize: bc.ARENA_SIZE,
    };
  }, sonicConfig);

  // The arena is five areas in two halves (clockwork/docs/ARENA.md).
  // clockwork's half: clockwork-state (METRICS, ..., SAMPLE_CLOCK,
  // CHANNEL_MAP), clockwork-audio (the SCOPE, then the TAPS), then
  // clockwork-transport (the ring cursors and the rings, the NRT-out ring at
  // the end). The guest's half follows: guest-public is the WINDOW, and
  // guest-private is GUEST_CONFIG (the world options) and, closing the arena,
  // GUEST_PERSIST (to the 16-byte alignment of the arena's end). A region is
  // APPENDED to the area it belongs to, so the seams pinned here are what
  // this file follows when one is added.
  expect(result.scopeStart + result.scopeTotalSize).toBeLessThanOrEqual(result.nrtOutStart);
  expect(result.nrtOutStart + result.nrtOutSize).toBeLessThanOrEqual(result.clockworkBlockSize);
  expect(result.clockworkBlockSize - (result.nrtOutStart + result.nrtOutSize)).toBeLessThan(16);
  expect(result.guestRegionStart).toBe(result.clockworkBlockSize);
  expect(result.windowStart).toBe(result.guestRegionStart);
  expect(result.worldOptionsStart).toBeGreaterThanOrEqual(result.windowStart + result.windowSize);
  expect(result.guestPersistStart).toBeGreaterThanOrEqual(result.worldOptionsStart + result.worldOptionsSize);
  expect(result.guestPersistStart + result.guestPersistSize).toBeLessThanOrEqual(result.totalBufferSize);
  expect(result.totalBufferSize - (result.guestPersistStart + result.guestPersistSize)).toBeLessThan(16);
  expect(result.channelMapStart + result.channelMapSize)
    .toBeLessThanOrEqual(result.nrtOutStart);
  expect(result.sampleClockStart + result.sampleClockSize)
    .toBeLessThanOrEqual(result.channelMapStart);
});
