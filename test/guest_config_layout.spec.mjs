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
 * room for the block, whole 32-bit slots, and a region that starts where the
 * ring buffers end.
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
      inBufferStart: bc.IN_BUFFER_START,
      inBufferSize: bc.IN_BUFFER_SIZE,
      worldOptionsStart: bc.GUEST_CONFIG_START,
      totalBufferSize: bc.TOTAL_BUFFER_SIZE,
    };
  }, sonicConfig);

  // GUEST_CONFIG_START must be beyond the IN ring buffer
  expect(result.worldOptionsStart).toBeGreaterThanOrEqual(
    result.inBufferStart + result.inBufferSize
  );
  // And must fit within total buffer
  expect(result.worldOptionsStart + 72).toBeLessThanOrEqual(result.totalBufferSize);
});

test("GUEST_CONFIG_START is consistent with TOTAL_BUFFER_SIZE", async ({ sonicPage, sonicConfig }) => {
  const result = await sonicPage.evaluate(async (config) => {
    const sonic = new window.SuperSonic(config);
    await sonic.init();

    const bc = sonic.bufferConstants;
    await sonic.shutdown();

    return {
      worldOptionsStart: bc.GUEST_CONFIG_START,
      worldOptionsSize: bc.GUEST_CONFIG_SIZE,
      scopeStart: bc.SHM_SCOPE_START,
      scopeTotalSize: bc.SHM_SCOPE_TOTAL_SIZE,
      sampleClockStart: bc.SAMPLE_CLOCK_START,
      sampleClockSize: bc.SAMPLE_CLOCK_SIZE,
      guestPersistStart: bc.GUEST_PERSIST_START,
      guestPersistSize: bc.GUEST_PERSIST_SIZE,
      channelMapStart: bc.CHANNEL_MAP_START,
      channelMapSize: bc.CHANNEL_MAP_SIZE,
      clientSlotsStart: bc.CLIENT_SLOTS_START,
      clientSlotsSize: bc.CLIENT_SLOTS_SIZE,
      clockworkBlockSize: bc.CLOCKWORK_BLOCK_SIZE,
      guestRegionStart: bc.GUEST_REGION_START,
      totalBufferSize: bc.TOTAL_BUFFER_SIZE,
    };
  }, sonicConfig);

  // The arena is two blocks (clockwork/js/lib/arena.js). clockwork's block
  // runs CONTROL, METRICS, ..., SAMPLE_CLOCK, CHANNEL_MAP, the rings, the
  // audio taps and ends with the CLIENT SLOTS. The guest's block follows,
  // opening with GUEST_CONFIG (the world options) and closing with
  // GUEST_PERSIST. Each region was APPENDED to its block, so the two tails are
  // what this file follows when a region is added.
  expect(result.clientSlotsStart + result.clientSlotsSize).toBe(result.clockworkBlockSize);
  expect(result.guestRegionStart).toBe(result.clockworkBlockSize);
  expect(result.worldOptionsStart).toBe(result.guestRegionStart);
  expect(result.guestPersistStart + result.guestPersistSize).toBe(result.totalBufferSize);
  expect(result.channelMapStart + result.channelMapSize)
    .toBeLessThanOrEqual(result.clientSlotsStart);
  expect(result.sampleClockStart + result.sampleClockSize)
    .toBeLessThanOrEqual(result.channelMapStart);
  expect(result.scopeStart).toBeGreaterThanOrEqual(result.worldOptionsStart + result.worldOptionsSize);
  expect(result.scopeStart + result.scopeTotalSize).toBeLessThanOrEqual(result.guestPersistStart);
});
