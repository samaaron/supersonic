/**
 * world_options_layout.spec.mjs — Buffer layout tests for WORLD_OPTIONS_START
 *
 * Validates that:
 *   - WORLD_OPTIONS_START is exposed in bufferConstants
 *   - WORLD_OPTIONS_START is outside the IN ring buffer
 *   - WORLD_OPTIONS_SIZE is correct (18 * 4 = 72 bytes)
 */
import { test, expect } from "./fixtures.mjs";

test("bufferConstants includes WORLD_OPTIONS_START and WORLD_OPTIONS_SIZE", async ({ sonicPage, sonicConfig }) => {
  const result = await sonicPage.evaluate(async (config) => {
    const sonic = new window.SuperSonic(config);
    await sonic.init();

    const bc = sonic.bufferConstants;
    await sonic.shutdown();

    return {
      hasStart: typeof bc.WORLD_OPTIONS_START === "number",
      hasSize: typeof bc.WORLD_OPTIONS_SIZE === "number",
      start: bc.WORLD_OPTIONS_START,
      size: bc.WORLD_OPTIONS_SIZE,
    };
  }, sonicConfig);

  expect(result.hasStart).toBe(true);
  expect(result.hasSize).toBe(true);
  expect(result.start).toBeGreaterThan(0);
  expect(result.size).toBe(72); // 18 * sizeof(uint32_t)
});

test("WORLD_OPTIONS_START is outside IN ring buffer", async ({ sonicPage, sonicConfig }) => {
  const result = await sonicPage.evaluate(async (config) => {
    const sonic = new window.SuperSonic(config);
    await sonic.init();

    const bc = sonic.bufferConstants;
    await sonic.shutdown();

    return {
      inBufferStart: bc.IN_BUFFER_START,
      inBufferSize: bc.IN_BUFFER_SIZE,
      worldOptionsStart: bc.WORLD_OPTIONS_START,
      totalBufferSize: bc.TOTAL_BUFFER_SIZE,
    };
  }, sonicConfig);

  // WORLD_OPTIONS_START must be beyond the IN ring buffer
  expect(result.worldOptionsStart).toBeGreaterThanOrEqual(
    result.inBufferStart + result.inBufferSize
  );
  // And must fit within total buffer
  expect(result.worldOptionsStart + 72).toBeLessThanOrEqual(result.totalBufferSize);
});

test("WORLD_OPTIONS_START is consistent with TOTAL_BUFFER_SIZE", async ({ sonicPage, sonicConfig }) => {
  const result = await sonicPage.evaluate(async (config) => {
    const sonic = new window.SuperSonic(config);
    await sonic.init();

    const bc = sonic.bufferConstants;
    await sonic.shutdown();

    return {
      worldOptionsStart: bc.WORLD_OPTIONS_START,
      worldOptionsSize: bc.WORLD_OPTIONS_SIZE,
      scopeStart: bc.SCOPE_START,
      scopeTotalSize: bc.SCOPE_TOTAL_SIZE,
      totalBufferSize: bc.TOTAL_BUFFER_SIZE,
    };
  }, sonicConfig);

  // Scope buffers are the last region before TOTAL_BUFFER_SIZE
  expect(result.scopeStart + result.scopeTotalSize).toBe(result.totalBufferSize);
  // WORLD_OPTIONS comes before the end of the buffer
  expect(result.worldOptionsStart + result.worldOptionsSize).toBeLessThan(result.totalBufferSize);
});
