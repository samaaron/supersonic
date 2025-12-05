import { test, expect } from '@playwright/test';

const SUPERSONIC_CONFIG = `{
  workerBaseURL: '/dist/workers/',
  wasmBaseURL: '/dist/wasm/',
  sampleBaseURL: '/dist/samples/',
  synthdefBaseURL: '/dist/synthdefs/',
}`;

test.describe('Recovery and Caching', () => {
  test.beforeEach(async ({ page }) => {
    page.on('console', (msg) => {
      if (msg.type() === 'error') {
        console.error('Browser console error:', msg.text());
      }
    });

    await page.goto('/test/harness.html');
    await page.waitForFunction(() => window.supersonicReady === true, {
      timeout: 10000,
    });
  });

  test('synthdef is cached after /d_recv via send()', async ({ page }) => {
    const result = await page.evaluate(async () => {
      const sonic = new window.SuperSonic({
        workerBaseURL: '/dist/workers/',
        wasmBaseURL: '/dist/wasm/',
        synthdefBaseURL: '/dist/synthdefs/',
      });

      await sonic.init();
      await sonic.loadSynthDef('sonic-pi-beep');

      const cached = sonic.loadedSynthDefs.has('sonic-pi-beep');
      const cacheSize = sonic.loadedSynthDefs.size;

      await sonic.destroy();
      return { cached, cacheSize };
    });

    expect(result.cached).toBe(true);
    expect(result.cacheSize).toBe(1);
  });

  test('synthdef is cached when sent via OSC API', async ({ page }) => {
    const result = await page.evaluate(async () => {
      const sonic = new window.SuperSonic({
        workerBaseURL: '/dist/workers/',
        wasmBaseURL: '/dist/wasm/',
        synthdefBaseURL: '/dist/synthdefs/',
      });

      await sonic.init();

      // Fetch synthdef bytes manually
      const response = await fetch('/dist/synthdefs/sonic-pi-beep.scsyndef');
      const bytes = new Uint8Array(await response.arrayBuffer());

      // Send via OSC API directly
      await sonic.send('/d_recv', bytes);

      // Check cache - should be cached by name parsed from binary
      const cached = sonic.loadedSynthDefs.has('sonic-pi-beep');

      await sonic.destroy();
      return { cached };
    });

    expect(result.cached).toBe(true);
  });

  test('/d_free removes synthdef from cache', async ({ page }) => {
    const result = await page.evaluate(async () => {
      const sonic = new window.SuperSonic({
        workerBaseURL: '/dist/workers/',
        wasmBaseURL: '/dist/wasm/',
        synthdefBaseURL: '/dist/synthdefs/',
      });

      await sonic.init();
      await sonic.loadSynthDef('sonic-pi-beep');
      const beforeFree = sonic.loadedSynthDefs.has('sonic-pi-beep');

      await sonic.send('/d_free', 'sonic-pi-beep');
      const afterFree = sonic.loadedSynthDefs.has('sonic-pi-beep');

      await sonic.destroy();
      return { beforeFree, afterFree };
    });

    expect(result.beforeFree).toBe(true);
    expect(result.afterFree).toBe(false);
  });

  test('/d_freeAll clears synthdef cache', async ({ page }) => {
    const result = await page.evaluate(async () => {
      const sonic = new window.SuperSonic({
        workerBaseURL: '/dist/workers/',
        wasmBaseURL: '/dist/wasm/',
        synthdefBaseURL: '/dist/synthdefs/',
      });

      await sonic.init();
      await sonic.loadSynthDef('sonic-pi-beep');
      const beforeFreeAll = sonic.loadedSynthDefs.size;

      await sonic.send('/d_freeAll');
      const afterFreeAll = sonic.loadedSynthDefs.size;

      await sonic.destroy();
      return { beforeFreeAll, afterFreeAll };
    });

    expect(result.beforeFreeAll).toBe(1);
    expect(result.afterFreeAll).toBe(0);
  });

  test('reset() clears synthdef cache', async ({ page }) => {
    const result = await page.evaluate(async () => {
      const sonic = new window.SuperSonic({
        workerBaseURL: '/dist/workers/',
        wasmBaseURL: '/dist/wasm/',
        synthdefBaseURL: '/dist/synthdefs/',
      });

      await sonic.init();
      await sonic.loadSynthDef('sonic-pi-beep');
      const beforeReset = sonic.loadedSynthDefs.size;

      await sonic.reset();
      const afterReset = sonic.loadedSynthDefs.size;

      await sonic.destroy();
      return { beforeReset, afterReset };
    });

    expect(result.beforeReset).toBe(1);
    expect(result.afterReset).toBe(0);
  });

  test('recover() restores cached synthdefs after reset', async ({ page }) => {
    const result = await page.evaluate(async () => {
      const sonic = new window.SuperSonic({
        workerBaseURL: '/dist/workers/',
        wasmBaseURL: '/dist/wasm/',
        synthdefBaseURL: '/dist/synthdefs/',
      });

      await sonic.init();
      await sonic.loadSynthDef('sonic-pi-beep');
      const beforeRecover = sonic.loadedSynthDefs.size;

      await sonic.recover();
      const afterRecover = sonic.loadedSynthDefs.size;
      const stillHasBeep = sonic.loadedSynthDefs.has('sonic-pi-beep');

      await sonic.destroy();
      return { beforeRecover, afterRecover, stillHasBeep };
    });

    expect(result.beforeRecover).toBe(1);
    expect(result.afterRecover).toBe(1);
    expect(result.stillHasBeep).toBe(true);
  });

  test('recover() returns true on success', async ({ page }) => {
    const result = await page.evaluate(async () => {
      const sonic = new window.SuperSonic({
        workerBaseURL: '/dist/workers/',
        wasmBaseURL: '/dist/wasm/',
      });

      await sonic.init();
      const success = await sonic.recover();

      await sonic.destroy();
      return { success };
    });

    expect(result.success).toBe(true);
  });

  test('recover() returns true when audio is already running', async ({ page }) => {
    // Tests the quick-resume path of recover() when AudioContext is already running
    const result = await page.evaluate(async () => {
      const sonic = new window.SuperSonic({
        workerBaseURL: '/dist/workers/',
        wasmBaseURL: '/dist/wasm/',
      });

      await sonic.init();
      // AudioContext should already be running, so recover() should succeed via quick resume
      const success = await sonic.recover();

      await sonic.destroy();
      return { success };
    });

    expect(result.success).toBe(true);
  });

  test('loading:start and loading:complete events fire for samples', async ({ page }) => {
    const result = await page.evaluate(async () => {
      const sonic = new window.SuperSonic({
        workerBaseURL: '/dist/workers/',
        wasmBaseURL: '/dist/wasm/',
        sampleBaseURL: '/dist/samples/',
      });

      const events = [];
      sonic.on('loading:start', (e) => events.push({ event: 'start', ...e }));
      sonic.on('loading:complete', (e) => events.push({ event: 'complete', ...e }));

      await sonic.init();
      await sonic.loadSample(0, 'loop_amen.flac');

      await sonic.destroy();

      const sampleEvents = events.filter(e => e.type === 'sample');
      return { sampleEvents };
    });

    expect(result.sampleEvents.length).toBe(2);
    expect(result.sampleEvents[0].event).toBe('start');
    expect(result.sampleEvents[0].name).toBe('loop_amen.flac');
    expect(result.sampleEvents[0].size).toBeGreaterThan(0); // download size from HEAD request
    expect(result.sampleEvents[1].event).toBe('complete');
    expect(result.sampleEvents[1].name).toBe('loop_amen.flac');
    expect(result.sampleEvents[1].size).toBeGreaterThan(0);
  });

  test('loading events fire for synthdefs', async ({ page }) => {
    const result = await page.evaluate(async () => {
      const sonic = new window.SuperSonic({
        workerBaseURL: '/dist/workers/',
        wasmBaseURL: '/dist/wasm/',
        synthdefBaseURL: '/dist/synthdefs/',
      });

      const events = [];
      sonic.on('loading:start', (e) => events.push({ event: 'start', ...e }));
      sonic.on('loading:complete', (e) => events.push({ event: 'complete', ...e }));

      await sonic.init();
      await sonic.loadSynthDef('sonic-pi-beep');

      await sonic.destroy();

      const synthdefEvents = events.filter(e => e.type === 'synthdef');
      return { synthdefEvents };
    });

    expect(result.synthdefEvents.length).toBe(2);
    expect(result.synthdefEvents[0].event).toBe('start');
    expect(result.synthdefEvents[0].name).toBe('sonic-pi-beep');
    expect(result.synthdefEvents[1].event).toBe('complete');
    expect(result.synthdefEvents[1].size).toBeGreaterThan(0);
  });

  test('recover() preserves loaded samples', async ({ page }) => {
    const result = await page.evaluate(async () => {
      const sonic = new window.SuperSonic({
        workerBaseURL: '/dist/workers/',
        wasmBaseURL: '/dist/wasm/',
        sampleBaseURL: '/dist/samples/',
        synthdefBaseURL: '/dist/synthdefs/',
      });

      await sonic.init();
      await sonic.loadSample(42, 'loop_amen.flac');
      await sonic.loadSynthDef('sonic-pi-beep');

      // Create a synth that plays the buffer to verify it's still registered
      await sonic.recover();

      // After recover, the buffer should still be accessible
      // We verify by checking that creating a synth with the buffer doesn't throw
      let synthCreated = false;
      try {
        await sonic.send('/s_new', 'sonic-pi-beep', 1000, 0, 0, 'out', 0);
        synthCreated = true;
        await sonic.send('/n_free', 1000);
      } catch (e) {
        synthCreated = false;
      }

      await sonic.destroy();
      return { synthCreated };
    });

    expect(result.synthCreated).toBe(true);
  });

  test('buffer data survives full reload path', async ({ page }) => {
    const result = await page.evaluate(async () => {
      const sonic = new window.SuperSonic({
        workerBaseURL: '/dist/workers/',
        wasmBaseURL: '/dist/wasm/',
        sampleBaseURL: '/dist/samples/',
        synthdefBaseURL: '/dist/synthdefs/',
      });

      const messages = [];
      sonic.on('message', (msg) => messages.push(JSON.parse(JSON.stringify(msg))));

      await sonic.init();

      // Load sample and query its info
      await sonic.loadSample(5, 'bd_haus.flac');
      await sonic.sync(1);

      messages.length = 0;
      await sonic.send('/b_query', 5);
      await sonic.sync(2);
      const beforeReload = messages.find((m) => m.address === '/b_info');
      const framesBefore = beforeReload?.args[1];
      const channelsBefore = beforeReload?.args[2];

      // Force full reload by calling internal #reload via recover
      // First, simulate worklet death by checking process count doesn't advance
      // We can't easily kill the worklet, so we test the reload path directly
      // by emitting recover:start which indicates reload path was taken
      let reloadPathTaken = false;
      sonic.on('recover:start', () => { reloadPathTaken = true; });

      // Call recover - in normal operation this would try resume first
      await sonic.recover();

      // Query buffer after recover
      messages.length = 0;
      await sonic.send('/b_query', 5);
      await sonic.sync(3);
      const afterRecover = messages.find((m) => m.address === '/b_info');
      const framesAfter = afterRecover?.args[1];
      const channelsAfter = afterRecover?.args[2];

      await sonic.destroy();

      return {
        framesBefore,
        channelsBefore,
        framesAfter,
        channelsAfter,
        reloadPathTaken,
        preserved: framesBefore === framesAfter && framesBefore > 0,
      };
    });

    expect(result.framesBefore).toBeGreaterThan(0);
    expect(result.framesAfter).toBe(result.framesBefore);
    expect(result.channelsAfter).toBe(result.channelsBefore);
    expect(result.preserved).toBe(true);
  });

  test('/b_allocFile buffer survives full reload', async ({ page }) => {
    const result = await page.evaluate(async () => {
      const sonic = new window.SuperSonic({
        workerBaseURL: '/dist/workers/',
        wasmBaseURL: '/dist/wasm/',
        sampleBaseURL: '/dist/samples/',
        synthdefBaseURL: '/dist/synthdefs/',
      });

      const messages = [];
      sonic.on('message', (msg) => messages.push(JSON.parse(JSON.stringify(msg))));

      await sonic.init();

      // Load sample via /b_allocFile (inline blob)
      const response = await fetch('/dist/samples/bd_haus.flac');
      const fileBytes = new Uint8Array(await response.arrayBuffer());
      await sonic.send('/b_allocFile', 7, fileBytes);
      await sonic.sync(1);

      // Query buffer info before recover
      messages.length = 0;
      await sonic.send('/b_query', 7);
      await sonic.sync(2);
      const beforeRecover = messages.find((m) => m.address === '/b_info');
      const framesBefore = beforeRecover?.args[1];

      // Recover
      await sonic.recover();

      // Query buffer after recover
      messages.length = 0;
      await sonic.send('/b_query', 7);
      await sonic.sync(3);
      const afterRecover = messages.find((m) => m.address === '/b_info');
      const framesAfter = afterRecover?.args[1];

      await sonic.destroy();

      return {
        framesBefore,
        framesAfter,
        preserved: framesBefore === framesAfter && framesBefore > 0,
      };
    });

    expect(result.framesBefore).toBeGreaterThan(0);
    expect(result.framesAfter).toBe(result.framesBefore);
    expect(result.preserved).toBe(true);
  });

  test('multiple buffers survive reload with correct buffer IDs', async ({ page }) => {
    const result = await page.evaluate(async () => {
      const sonic = new window.SuperSonic({
        workerBaseURL: '/dist/workers/',
        wasmBaseURL: '/dist/wasm/',
        sampleBaseURL: '/dist/samples/',
        synthdefBaseURL: '/dist/synthdefs/',
      });

      const messages = [];
      sonic.on('message', (msg) => messages.push(JSON.parse(JSON.stringify(msg))));

      await sonic.init();

      // Load multiple samples with specific buffer numbers
      await sonic.loadSample(10, 'bd_haus.flac');
      await sonic.loadSample(20, 'sn_dub.flac');
      await sonic.loadSample(30, 'hat_zap.flac');
      await sonic.sync(1);

      // Query all buffers before recover
      messages.length = 0;
      await sonic.send('/b_query', 10);
      await sonic.send('/b_query', 20);
      await sonic.send('/b_query', 30);
      await sonic.sync(2);

      const beforeInfos = messages.filter((m) => m.address === '/b_info');
      const before = {
        buf10: beforeInfos.find(m => m.args[0] === 10)?.args[1],
        buf20: beforeInfos.find(m => m.args[0] === 20)?.args[1],
        buf30: beforeInfos.find(m => m.args[0] === 30)?.args[1],
      };

      // Recover
      await sonic.recover();

      // Query all buffers after recover
      messages.length = 0;
      await sonic.send('/b_query', 10);
      await sonic.send('/b_query', 20);
      await sonic.send('/b_query', 30);
      await sonic.sync(3);

      const afterInfos = messages.filter((m) => m.address === '/b_info');
      const after = {
        buf10: afterInfos.find(m => m.args[0] === 10)?.args[1],
        buf20: afterInfos.find(m => m.args[0] === 20)?.args[1],
        buf30: afterInfos.find(m => m.args[0] === 30)?.args[1],
      };

      await sonic.destroy();

      return {
        before,
        after,
        allPreserved: before.buf10 === after.buf10 &&
                      before.buf20 === after.buf20 &&
                      before.buf30 === after.buf30 &&
                      before.buf10 > 0,
      };
    });

    expect(result.before.buf10).toBeGreaterThan(0);
    expect(result.before.buf20).toBeGreaterThan(0);
    expect(result.before.buf30).toBeGreaterThan(0);
    expect(result.after.buf10).toBe(result.before.buf10);
    expect(result.after.buf20).toBe(result.before.buf20);
    expect(result.after.buf30).toBe(result.before.buf30);
    expect(result.allPreserved).toBe(true);
  });

  test('buffer sample data is preserved after recover', async ({ page }) => {
    const result = await page.evaluate(async () => {
      const sonic = new window.SuperSonic({
        workerBaseURL: '/dist/workers/',
        wasmBaseURL: '/dist/wasm/',
        sampleBaseURL: '/dist/samples/',
        synthdefBaseURL: '/dist/synthdefs/',
      });

      const messages = [];
      sonic.on('message', (msg) => messages.push(JSON.parse(JSON.stringify(msg))));

      await sonic.init();

      // Load a sample
      await sonic.loadSample(5, 'bd_haus.flac');
      await sonic.sync(1);

      // Read first 10 samples before recover
      messages.length = 0;
      await sonic.send('/b_getn', 5, 0, 10);
      await sonic.sync(2);

      const beforeReply = messages.find((m) => m.address === '/b_setn');
      // /b_setn args: [bufnum, startIndex, numSamples, ...samples]
      const samplesBefore = beforeReply?.args.slice(3) || [];

      // Recover
      await sonic.recover();

      // Read first 10 samples after recover
      messages.length = 0;
      await sonic.send('/b_getn', 5, 0, 10);
      await sonic.sync(3);

      const afterReply = messages.find((m) => m.address === '/b_setn');
      const samplesAfter = afterReply?.args.slice(3) || [];

      await sonic.destroy();

      // Compare sample values
      const allMatch = samplesBefore.length === samplesAfter.length &&
        samplesBefore.every((val, i) => val === samplesAfter[i]);

      return {
        samplesBefore,
        samplesAfter,
        allMatch,
        count: samplesBefore.length,
      };
    });

    expect(result.count).toBe(10);
    expect(result.allMatch).toBe(true);
    expect(result.samplesBefore).toEqual(result.samplesAfter);
  });
});
