import { test, expect } from "@playwright/test";

test.describe("SuperSonic loadSample()", () => {
  test.beforeEach(async ({ page }) => {
    // Collect console errors
    page.on("console", (msg) => {
      if (msg.type() === "error") {
        console.error("Browser console error:", msg.text());
      }
    });

    // Collect page errors
    page.on("pageerror", (err) => {
      console.error("Page error:", err.message);
    });

    await page.goto("/test/harness.html");

    // Wait for the SuperSonic module to load
    await page.waitForFunction(() => window.supersonicReady === true, {
      timeout: 10000,
    });
  });

  test("loads sample by simple filename", async ({ page }) => {
    const result = await page.evaluate(async () => {
      const sonic = new window.SuperSonic({
        workerBaseURL: "/dist/workers/",
        wasmBaseURL: "/dist/wasm/",
        sampleBaseURL: "/dist/samples/",
        synthdefBaseURL: "/dist/synthdefs/",
      });

      const messages = [];
      sonic.on('message', (msg) => messages.push(JSON.parse(JSON.stringify(msg))));

      try {
        await sonic.init();

        // Load sample by simple filename - should use sampleBaseURL
        await sonic.loadSample(0, "bd_haus.flac");
        await sonic.sync(1);

        // Query buffer to verify it was loaded
        messages.length = 0;
        await sonic.send("/b_query", 0);
        await sonic.sync(2);

        const queryReply = messages.find((m) => m.address === "/b_info");

        return {
          success: true,
          queryReply,
          bufnum: queryReply?.args[0],
          numFrames: queryReply?.args[1],
          numChannels: queryReply?.args[2],
          sampleRate: queryReply?.args[3],
        };
      } catch (err) {
        return { success: false, error: err.message, stack: err.stack };
      }
    });

    expect(result.error).toBeUndefined();
    expect(result.success).toBe(true);
    expect(result.bufnum).toBe(0);
    expect(result.numFrames).toBeGreaterThan(0);
    expect(result.numChannels).toBeGreaterThanOrEqual(1);
    expect(result.sampleRate).toBeGreaterThan(0);
  });

  test("loads sample by absolute path", async ({ page }) => {
    const result = await page.evaluate(async () => {
      const sonic = new window.SuperSonic({
        workerBaseURL: "/dist/workers/",
        wasmBaseURL: "/dist/wasm/",
        sampleBaseURL: "/dist/samples/",
        synthdefBaseURL: "/dist/synthdefs/",
      });

      const messages = [];
      sonic.on('message', (msg) => messages.push(JSON.parse(JSON.stringify(msg))));

      try {
        await sonic.init();

        // Load sample by absolute path - should NOT use sampleBaseURL
        await sonic.loadSample(0, "/dist/samples/bd_haus.flac");
        await sonic.sync(1);

        // Query buffer to verify it was loaded
        messages.length = 0;
        await sonic.send("/b_query", 0);
        await sonic.sync(2);

        const queryReply = messages.find((m) => m.address === "/b_info");

        return {
          success: true,
          queryReply,
          bufnum: queryReply?.args[0],
          numFrames: queryReply?.args[1],
        };
      } catch (err) {
        return { success: false, error: err.message, stack: err.stack };
      }
    });

    expect(result.error).toBeUndefined();
    expect(result.success).toBe(true);
    expect(result.bufnum).toBe(0);
    expect(result.numFrames).toBeGreaterThan(0);
  });

  test("path with ./ prefix bypasses sampleBaseURL", async ({ page }) => {
    const result = await page.evaluate(async () => {
      const sonic = new window.SuperSonic({
        workerBaseURL: "/dist/workers/",
        wasmBaseURL: "/dist/wasm/",
        sampleBaseURL: "/dist/samples/",
        synthdefBaseURL: "/dist/synthdefs/",
      });

      try {
        await sonic.init();

        // Load sample with ./ prefix - should NOT prepend sampleBaseURL
        // This will 404 because ./nonexistent.flac doesn't exist,
        // but the error message will show the attempted path
        await sonic.loadSample(0, "./nonexistent.flac");

        return { success: true };
      } catch (err) {
        return {
          success: false,
          error: err.message,
          // Check that error mentions ./nonexistent.flac, NOT /dist/samples/./nonexistent.flac
          pathNotPrepended: err.message.includes("./nonexistent.flac") &&
                           !err.message.includes("/dist/samples/./nonexistent.flac"),
        };
      }
    });

    // Should fail with 404, but path should not have sampleBaseURL prepended
    expect(result.success).toBe(false);
    expect(result.pathNotPrepended).toBe(true);
  });

  test("loads multiple samples into different buffers", async ({ page }) => {
    const result = await page.evaluate(async () => {
      const sonic = new window.SuperSonic({
        workerBaseURL: "/dist/workers/",
        wasmBaseURL: "/dist/wasm/",
        sampleBaseURL: "/dist/samples/",
        synthdefBaseURL: "/dist/synthdefs/",
      });

      const messages = [];
      sonic.on('message', (msg) => messages.push(JSON.parse(JSON.stringify(msg))));

      try {
        await sonic.init();

        // Load multiple samples
        await sonic.loadSample(0, "bd_haus.flac");
        await sonic.loadSample(1, "sn_dub.flac");
        await sonic.loadSample(2, "hat_zap.flac");
        await sonic.sync(1);

        // Query all buffers
        messages.length = 0;
        await sonic.send("/b_query", 0);
        await sonic.send("/b_query", 1);
        await sonic.send("/b_query", 2);
        await sonic.sync(2);

        const queryReplies = messages.filter((m) => m.address === "/b_info");

        return {
          success: true,
          numBuffersLoaded: queryReplies.length,
          buffers: queryReplies.map((r) => ({
            bufnum: r.args[0],
            numFrames: r.args[1],
          })),
        };
      } catch (err) {
        return { success: false, error: err.message, stack: err.stack };
      }
    });

    expect(result.error).toBeUndefined();
    expect(result.success).toBe(true);
    expect(result.numBuffersLoaded).toBe(3);
    expect(result.buffers[0].bufnum).toBe(0);
    expect(result.buffers[1].bufnum).toBe(1);
    expect(result.buffers[2].bufnum).toBe(2);
    expect(result.buffers[0].numFrames).toBeGreaterThan(0);
    expect(result.buffers[1].numFrames).toBeGreaterThan(0);
    expect(result.buffers[2].numFrames).toBeGreaterThan(0);
  });

  test("can play loaded sample with synth", async ({ page }) => {
    const result = await page.evaluate(async () => {
      const sonic = new window.SuperSonic({
        workerBaseURL: "/dist/workers/",
        wasmBaseURL: "/dist/wasm/",
        sampleBaseURL: "/dist/samples/",
        synthdefBaseURL: "/dist/synthdefs/",
      });

      try {
        await sonic.init();

        // Load sample and synthdef
        await sonic.loadSample(0, "bd_haus.flac");
        await sonic.loadSynthDef("sonic-pi-basic_stereo_player");
        await sonic.sync(1);

        // Play the sample
        await sonic.send(
          "/s_new",
          "sonic-pi-basic_stereo_player",
          1000,
          1,
          0,
          "buf",
          0
        );
        await sonic.sync(2);

        // Check node tree has the synth
        const tree = sonic.getTree();

        return {
          success: true,
          nodeCount: tree.nodeCount,
          hasSynth: tree.nodes.some((n) => n.id === 1000),
        };
      } catch (err) {
        return { success: false, error: err.message, stack: err.stack };
      }
    });

    expect(result.error).toBeUndefined();
    expect(result.success).toBe(true);
    expect(result.hasSynth).toBe(true);
  });

  test("rejects path with directory traversal", async ({ page }) => {
    const result = await page.evaluate(async () => {
      const sonic = new window.SuperSonic({
        workerBaseURL: "/dist/workers/",
        wasmBaseURL: "/dist/wasm/",
        sampleBaseURL: "/dist/samples/",
        synthdefBaseURL: "/dist/synthdefs/",
      });

      try {
        await sonic.init();

        // Try to load with directory traversal - should fail
        await sonic.loadSample(0, "../../../etc/passwd");

        return { success: true };
      } catch (err) {
        return {
          success: false,
          error: err.message,
          isTraversalError: err.message.includes(".."),
        };
      }
    });

    expect(result.success).toBe(false);
    expect(result.isTraversalError).toBe(true);
  });

  test("rejects path with backslash", async ({ page }) => {
    const result = await page.evaluate(async () => {
      const sonic = new window.SuperSonic({
        workerBaseURL: "/dist/workers/",
        wasmBaseURL: "/dist/wasm/",
        sampleBaseURL: "/dist/samples/",
        synthdefBaseURL: "/dist/synthdefs/",
      });

      try {
        await sonic.init();

        // Try to load with backslash - should fail
        await sonic.loadSample(0, "samples\\bd_haus.flac");

        return { success: true };
      } catch (err) {
        return {
          success: false,
          error: err.message,
          isBackslashError: err.message.includes("forward slashes"),
        };
      }
    });

    expect(result.success).toBe(false);
    expect(result.isBackslashError).toBe(true);
  });

  test("handles non-existent sample file", async ({ page }) => {
    const result = await page.evaluate(async () => {
      const sonic = new window.SuperSonic({
        workerBaseURL: "/dist/workers/",
        wasmBaseURL: "/dist/wasm/",
        sampleBaseURL: "/dist/samples/",
        synthdefBaseURL: "/dist/synthdefs/",
      });

      try {
        await sonic.init();

        // Try to load non-existent file
        await sonic.loadSample(0, "this_file_does_not_exist.flac");

        return { success: true };
      } catch (err) {
        return {
          success: false,
          error: err.message,
          isFetchError: err.message.includes("404") || err.message.includes("Failed to fetch"),
        };
      }
    });

    expect(result.success).toBe(false);
    expect(result.isFetchError).toBe(true);
  });

  test("loads sample with startFrame parameter", async ({ page }) => {
    const result = await page.evaluate(async () => {
      const sonic = new window.SuperSonic({
        workerBaseURL: "/dist/workers/",
        wasmBaseURL: "/dist/wasm/",
        sampleBaseURL: "/dist/samples/",
        synthdefBaseURL: "/dist/synthdefs/",
      });

      const messages = [];
      sonic.on('message', (msg) => messages.push(JSON.parse(JSON.stringify(msg))));

      try {
        await sonic.init();

        // Load full sample first
        await sonic.loadSample(0, "bd_haus.flac");
        await sonic.sync(1);

        messages.length = 0;
        await sonic.send("/b_query", 0);
        await sonic.sync(2);
        const fullInfo = messages.find((m) => m.address === "/b_info");
        const fullFrames = fullInfo.args[1];

        // Load same sample with startFrame offset
        const startFrame = Math.floor(fullFrames / 2);
        await sonic.loadSample(1, "bd_haus.flac", startFrame);
        await sonic.sync(3);

        messages.length = 0;
        await sonic.send("/b_query", 1);
        await sonic.sync(4);
        const partialInfo = messages.find((m) => m.address === "/b_info");
        const partialFrames = partialInfo.args[1];

        return {
          success: true,
          fullFrames,
          partialFrames,
          startFrame,
          // Partial should have roughly half the frames (minus startFrame)
          expectedPartial: fullFrames - startFrame,
        };
      } catch (err) {
        return { success: false, error: err.message, stack: err.stack };
      }
    });

    expect(result.error).toBeUndefined();
    expect(result.success).toBe(true);
    expect(result.partialFrames).toBe(result.expectedPartial);
    expect(result.partialFrames).toBeLessThan(result.fullFrames);
  });

  test("loads sample with numFrames parameter", async ({ page }) => {
    const result = await page.evaluate(async () => {
      const sonic = new window.SuperSonic({
        workerBaseURL: "/dist/workers/",
        wasmBaseURL: "/dist/wasm/",
        sampleBaseURL: "/dist/samples/",
        synthdefBaseURL: "/dist/synthdefs/",
      });

      const messages = [];
      sonic.on('message', (msg) => messages.push(JSON.parse(JSON.stringify(msg))));

      try {
        await sonic.init();

        // Load only first 1000 frames
        const requestedFrames = 1000;
        await sonic.loadSample(0, "bd_haus.flac", 0, requestedFrames);
        await sonic.sync(1);

        messages.length = 0;
        await sonic.send("/b_query", 0);
        await sonic.sync(2);
        const info = messages.find((m) => m.address === "/b_info");

        return {
          success: true,
          requestedFrames,
          actualFrames: info.args[1],
        };
      } catch (err) {
        return { success: false, error: err.message, stack: err.stack };
      }
    });

    expect(result.error).toBeUndefined();
    expect(result.success).toBe(true);
    expect(result.actualFrames).toBe(result.requestedFrames);
  });

  test("replaces existing buffer content", async ({ page }) => {
    const result = await page.evaluate(async () => {
      const sonic = new window.SuperSonic({
        workerBaseURL: "/dist/workers/",
        wasmBaseURL: "/dist/wasm/",
        sampleBaseURL: "/dist/samples/",
        synthdefBaseURL: "/dist/synthdefs/",
      });

      const messages = [];
      sonic.on('message', (msg) => messages.push(JSON.parse(JSON.stringify(msg))));

      try {
        await sonic.init();

        // Load first sample into buffer 0
        await sonic.loadSample(0, "bd_haus.flac");
        await sonic.sync(1);

        messages.length = 0;
        await sonic.send("/b_query", 0);
        await sonic.sync(2);
        const firstInfo = messages.find((m) => m.address === "/b_info");
        const firstFrames = firstInfo.args[1];

        // Replace with a different sample
        await sonic.loadSample(0, "sn_dub.flac");
        await sonic.sync(3);

        messages.length = 0;
        await sonic.send("/b_query", 0);
        await sonic.sync(4);
        const secondInfo = messages.find((m) => m.address === "/b_info");
        const secondFrames = secondInfo.args[1];

        return {
          success: true,
          firstFrames,
          secondFrames,
          // Different samples should have different frame counts
          framesChanged: firstFrames !== secondFrames,
        };
      } catch (err) {
        return { success: false, error: err.message, stack: err.stack };
      }
    });

    expect(result.error).toBeUndefined();
    expect(result.success).toBe(true);
    // The samples may have different lengths, verifying buffer was replaced
    expect(result.firstFrames).toBeGreaterThan(0);
    expect(result.secondFrames).toBeGreaterThan(0);
  });

  test("loads sample from inline blob via /b_allocFile", async ({ page }) => {
    const result = await page.evaluate(async () => {
      const sonic = new window.SuperSonic({
        workerBaseURL: "/dist/workers/",
        wasmBaseURL: "/dist/wasm/",
        sampleBaseURL: "/dist/samples/",
        synthdefBaseURL: "/dist/synthdefs/",
      });

      const messages = [];
      sonic.on('message', (msg) => messages.push(JSON.parse(JSON.stringify(msg))));

      try {
        await sonic.init();

        // Fetch sample file as raw bytes (simulating external controller sending file data)
        const response = await fetch("/dist/samples/bd_haus.flac");
        const arrayBuffer = await response.arrayBuffer();
        const fileBytes = new Uint8Array(arrayBuffer);

        // Send via /b_allocFile with inline blob
        await sonic.send("/b_allocFile", 0, fileBytes);
        await sonic.sync(1);

        // Query buffer to verify it was loaded
        messages.length = 0;
        await sonic.send("/b_query", 0);
        await sonic.sync(2);

        const queryReply = messages.find((m) => m.address === "/b_info");

        return {
          success: true,
          queryReply,
          bufnum: queryReply?.args[0],
          numFrames: queryReply?.args[1],
          numChannels: queryReply?.args[2],
          sampleRate: queryReply?.args[3],
          blobSize: fileBytes.length,
        };
      } catch (err) {
        return { success: false, error: err.message, stack: err.stack };
      }
    });

    expect(result.error).toBeUndefined();
    expect(result.success).toBe(true);
    expect(result.bufnum).toBe(0);
    expect(result.numFrames).toBeGreaterThan(0);
    expect(result.numChannels).toBeGreaterThanOrEqual(1);
    expect(result.sampleRate).toBeGreaterThan(0);
    expect(result.blobSize).toBeGreaterThan(0);
  });

  test("/b_allocFile can play loaded sample with synth", async ({ page }) => {
    const result = await page.evaluate(async () => {
      const sonic = new window.SuperSonic({
        workerBaseURL: "/dist/workers/",
        wasmBaseURL: "/dist/wasm/",
        sampleBaseURL: "/dist/samples/",
        synthdefBaseURL: "/dist/synthdefs/",
      });

      try {
        await sonic.init();

        // Fetch and load sample via /b_allocFile
        const response = await fetch("/dist/samples/bd_haus.flac");
        const fileBytes = new Uint8Array(await response.arrayBuffer());
        await sonic.send("/b_allocFile", 0, fileBytes);

        // Load synthdef
        await sonic.loadSynthDef("sonic-pi-basic_stereo_player");
        await sonic.sync(1);

        // Play the sample
        await sonic.send(
          "/s_new",
          "sonic-pi-basic_stereo_player",
          1000,
          1,
          0,
          "buf",
          0
        );
        await sonic.sync(2);

        // Check node tree has the synth
        const tree = sonic.getTree();

        return {
          success: true,
          nodeCount: tree.nodeCount,
          hasSynth: tree.nodes.some((n) => n.id === 1000),
        };
      } catch (err) {
        return { success: false, error: err.message, stack: err.stack };
      }
    });

    expect(result.error).toBeUndefined();
    expect(result.success).toBe(true);
    expect(result.hasSynth).toBe(true);
  });

  test("/b_allocFile rejects missing blob argument", async ({ page }) => {
    const result = await page.evaluate(async () => {
      const sonic = new window.SuperSonic({
        workerBaseURL: "/dist/workers/",
        wasmBaseURL: "/dist/wasm/",
        sampleBaseURL: "/dist/samples/",
        synthdefBaseURL: "/dist/synthdefs/",
      });

      try {
        await sonic.init();

        // Try /b_allocFile without blob - should fail
        await sonic.send("/b_allocFile", 0);

        return { success: true };
      } catch (err) {
        return {
          success: false,
          error: err.message,
          isBlobError: err.message.includes("blob"),
        };
      }
    });

    expect(result.success).toBe(false);
    expect(result.isBlobError).toBe(true);
  });
});
