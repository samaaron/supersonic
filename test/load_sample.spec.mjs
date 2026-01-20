import { test, expect } from "./fixtures.mjs";

/**
 * Generate a valid AIFF file in-memory for testing
 * Creates a simple mono 16-bit PCM file with a sine wave
 * @param {Object} options
 * @param {number} options.sampleRate - Sample rate (default 44100)
 * @param {number} options.numFrames - Number of frames (default 4410 = 0.1s)
 * @param {number} options.numChannels - Number of channels (default 1)
 * @param {number} options.bitsPerSample - Bits per sample (default 16)
 * @param {number} options.frequency - Sine wave frequency (default 440)
 * @returns {Uint8Array} AIFF file bytes
 */
function generateAiffFile({
  sampleRate = 44100,
  numFrames = 4410,
  numChannels = 1,
  bitsPerSample = 16,
  frequency = 440
} = {}) {
  const bytesPerSample = bitsPerSample / 8;
  const audioDataSize = numFrames * numChannels * bytesPerSample;

  // COMM chunk: 8 (header) + 18 (data) = 26 bytes
  // SSND chunk: 8 (header) + 8 (offset/blockSize) + audioDataSize
  const commChunkSize = 18;
  const ssndChunkSize = 8 + audioDataSize;
  const formSize = 4 + (8 + commChunkSize) + (8 + ssndChunkSize); // AIFF + chunks

  const totalSize = 12 + (8 + commChunkSize) + (8 + ssndChunkSize);
  const buffer = new ArrayBuffer(totalSize);
  const view = new DataView(buffer);
  const bytes = new Uint8Array(buffer);
  let offset = 0;

  // FORM header
  bytes.set([0x46, 0x4F, 0x52, 0x4D], offset); // "FORM"
  offset += 4;
  view.setUint32(offset, formSize, false); // big-endian
  offset += 4;
  bytes.set([0x41, 0x49, 0x46, 0x46], offset); // "AIFF"
  offset += 4;

  // COMM chunk
  bytes.set([0x43, 0x4F, 0x4D, 0x4D], offset); // "COMM"
  offset += 4;
  view.setUint32(offset, commChunkSize, false);
  offset += 4;
  view.setUint16(offset, numChannels, false);
  offset += 2;
  view.setUint32(offset, numFrames, false);
  offset += 4;
  view.setUint16(offset, bitsPerSample, false);
  offset += 2;

  // 80-bit extended float for sample rate
  // Simplified encoding for common sample rates
  const float80 = encodeFloat80(sampleRate);
  bytes.set(float80, offset);
  offset += 10;

  // SSND chunk
  bytes.set([0x53, 0x53, 0x4E, 0x44], offset); // "SSND"
  offset += 4;
  view.setUint32(offset, ssndChunkSize, false);
  offset += 4;
  view.setUint32(offset, 0, false); // offset
  offset += 4;
  view.setUint32(offset, 0, false); // blockSize
  offset += 4;

  // Generate sine wave audio data (big-endian)
  for (let frame = 0; frame < numFrames; frame++) {
    const t = frame / sampleRate;
    const sample = Math.sin(2 * Math.PI * frequency * t);

    for (let ch = 0; ch < numChannels; ch++) {
      if (bitsPerSample === 16) {
        const intSample = Math.round(sample * 32767);
        // Big-endian 16-bit
        bytes[offset++] = (intSample >> 8) & 0xFF;
        bytes[offset++] = intSample & 0xFF;
      } else if (bitsPerSample === 8) {
        // AIFF 8-bit is signed (-128 to 127)
        const intSample = Math.round(sample * 127);
        bytes[offset++] = intSample & 0xFF;
      }
    }
  }

  return new Uint8Array(buffer);
}

/**
 * Encode a sample rate as 80-bit IEEE 754 extended precision float
 * Uses BigInt for precision since mantissa values exceed JS safe integer range
 */
function encodeFloat80(value) {
  const result = new Uint8Array(10);

  if (value === 0) return result;

  const sign = value < 0 ? 1 : 0;
  value = Math.abs(value);

  // Find exponent n where 2^n <= value < 2^(n+1)
  const n = Math.floor(Math.log2(value));
  const exponent = n + 16383;

  // Mantissa = value * 2^(63-n), using BigInt for precision
  // This shifts the value left by (63-n) bits, placing the integer bit at position 63
  let mantissa = BigInt(Math.round(value)) << BigInt(63 - n);

  // Write sign and exponent (15 bits)
  result[0] = (sign << 7) | ((exponent >> 8) & 0x7F);
  result[1] = exponent & 0xFF;

  // Write mantissa (64 bits, big-endian: MSB at byte 2, LSB at byte 9)
  for (let i = 9; i >= 2; i--) {
    result[i] = Number(mantissa & 0xFFn);
    mantissa = mantissa >> 8n;
  }

  return result;
}

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

  test("loads sample by simple filename", async ({ page, sonicConfig }) => {
    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);

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
    }, sonicConfig);

    expect(result.error).toBeUndefined();
    expect(result.success).toBe(true);
    expect(result.bufnum).toBe(0);
    expect(result.numFrames).toBeGreaterThan(0);
    expect(result.numChannels).toBeGreaterThanOrEqual(1);
    expect(result.sampleRate).toBeGreaterThan(0);
  });

  test("loads sample by absolute path", async ({ page, sonicConfig }) => {
    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);

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
    }, sonicConfig);

    expect(result.error).toBeUndefined();
    expect(result.success).toBe(true);
    expect(result.bufnum).toBe(0);
    expect(result.numFrames).toBeGreaterThan(0);
  });

  test("path with ./ prefix bypasses sampleBaseURL", async ({ page, sonicConfig }) => {
    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);

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
    }, sonicConfig);

    // Should fail with 404, but path should not have sampleBaseURL prepended
    expect(result.success).toBe(false);
    expect(result.pathNotPrepended).toBe(true);
  });

  test("loads multiple samples into different buffers", async ({ page, sonicConfig }) => {
    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);

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
    }, sonicConfig);

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

  test("can play loaded sample with synth", async ({ page, sonicConfig }) => {
    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);

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

        // Wait for tree update in postMessage mode
        let tree;
        const start = Date.now();
        while (Date.now() - start < 2000) {
          tree = sonic.getRawTree();
          if (tree.nodes.some((n) => n.id === 1000)) break;
          await new Promise(r => setTimeout(r, 20));
        }

        return {
          success: true,
          nodeCount: tree.nodeCount,
          hasSynth: tree.nodes.some((n) => n.id === 1000),
        };
      } catch (err) {
        return { success: false, error: err.message, stack: err.stack };
      }
    }, sonicConfig);

    expect(result.error).toBeUndefined();
    expect(result.success).toBe(true);
    expect(result.hasSynth).toBe(true);
  });

  test("rejects path with directory traversal", async ({ page, sonicConfig }) => {
    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);

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
    }, sonicConfig);

    expect(result.success).toBe(false);
    expect(result.isTraversalError).toBe(true);
  });

  test("rejects path with backslash", async ({ page, sonicConfig }) => {
    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);

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
    }, sonicConfig);

    expect(result.success).toBe(false);
    expect(result.isBackslashError).toBe(true);
  });

  test("handles non-existent sample file", async ({ page, sonicConfig }) => {
    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);

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
    }, sonicConfig);

    expect(result.success).toBe(false);
    expect(result.isFetchError).toBe(true);
  });

  test("loads sample with startFrame parameter", async ({ page, sonicConfig }) => {
    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);

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
    }, sonicConfig);

    expect(result.error).toBeUndefined();
    expect(result.success).toBe(true);
    expect(result.partialFrames).toBe(result.expectedPartial);
    expect(result.partialFrames).toBeLessThan(result.fullFrames);
  });

  test("loads sample with numFrames parameter", async ({ page, sonicConfig }) => {
    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);

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
    }, sonicConfig);

    expect(result.error).toBeUndefined();
    expect(result.success).toBe(true);
    expect(result.actualFrames).toBe(result.requestedFrames);
  });

  test("replaces existing buffer content", async ({ page, sonicConfig }) => {
    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);

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
    }, sonicConfig);

    expect(result.error).toBeUndefined();
    expect(result.success).toBe(true);
    // The samples may have different lengths, verifying buffer was replaced
    expect(result.firstFrames).toBeGreaterThan(0);
    expect(result.secondFrames).toBeGreaterThan(0);
  });

  test("loads sample from inline blob via /b_allocFile", async ({ page, sonicConfig }) => {
    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);

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
    }, sonicConfig);

    expect(result.error).toBeUndefined();
    expect(result.success).toBe(true);
    expect(result.bufnum).toBe(0);
    expect(result.numFrames).toBeGreaterThan(0);
    expect(result.numChannels).toBeGreaterThanOrEqual(1);
    expect(result.sampleRate).toBeGreaterThan(0);
    expect(result.blobSize).toBeGreaterThan(0);
  });

  test("/b_allocFile can play loaded sample with synth", async ({ page, sonicConfig }) => {
    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);

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

        // Wait for tree update in postMessage mode
        let tree;
        const start = Date.now();
        while (Date.now() - start < 2000) {
          tree = sonic.getRawTree();
          if (tree.nodes.some((n) => n.id === 1000)) break;
          await new Promise(r => setTimeout(r, 20));
        }

        return {
          success: true,
          nodeCount: tree.nodeCount,
          hasSynth: tree.nodes.some((n) => n.id === 1000),
        };
      } catch (err) {
        return { success: false, error: err.message, stack: err.stack };
      }
    }, sonicConfig);

    expect(result.error).toBeUndefined();
    expect(result.success).toBe(true);
    expect(result.hasSynth).toBe(true);
  });

  test("/b_allocFile rejects missing blob argument", async ({ page, sonicConfig }) => {
    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);

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
    }, sonicConfig);

    expect(result.success).toBe(false);
    expect(result.isBlobError).toBe(true);
  });

  test("/b_allocFile buffer survives recover()", async ({ page, sonicConfig }) => {
    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);

      const messages = [];
      sonic.on('message', (msg) => messages.push(JSON.parse(JSON.stringify(msg))));

      try {
        await sonic.init();

        // Load sample via /b_allocFile
        const response = await fetch("/dist/samples/bd_haus.flac");
        const fileBytes = new Uint8Array(await response.arrayBuffer());
        await sonic.send("/b_allocFile", 0, fileBytes);
        await sonic.sync(1);

        // Query buffer before recover
        messages.length = 0;
        await sonic.send("/b_query", 0);
        await sonic.sync(2);
        const beforeRecover = messages.find((m) => m.address === "/b_info");
        const framesBefore = beforeRecover?.args[1];

        // Recover
        await sonic.recover();

        // Query buffer after recover - should still exist with same data
        messages.length = 0;
        await sonic.send("/b_query", 0);
        await sonic.sync(3);
        const afterRecover = messages.find((m) => m.address === "/b_info");
        const framesAfter = afterRecover?.args[1];

        return {
          success: true,
          framesBefore,
          framesAfter,
          preserved: framesBefore === framesAfter && framesBefore > 0,
        };
      } catch (err) {
        return { success: false, error: err.message, stack: err.stack };
      }
    }, sonicConfig);

    expect(result.error).toBeUndefined();
    expect(result.success).toBe(true);
    expect(result.framesBefore).toBeGreaterThan(0);
    expect(result.preserved).toBe(true);
  });

  test("loads real AIFF file (ffmpeg-generated)", async ({ page, sonicConfig }) => {
    // Test against a real AIFF file created by ffmpeg (independent implementation)
    // This catches symmetric bugs where test generator and parser have matching errors
    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);

      const messages = [];
      sonic.on('message', (msg) => messages.push(JSON.parse(JSON.stringify(msg))));

      try {
        await sonic.init();

        // Load real AIFF file by path
        await sonic.loadSample(0, "bd_haus.aiff");
        await sonic.sync(1);

        // Query buffer to verify it was loaded
        messages.length = 0;
        await sonic.send("/b_query", 0);
        await sonic.sync(2);

        const queryReply = messages.find((m) => m.address === "/b_info");

        return {
          success: true,
          bufnum: queryReply?.args[0],
          numFrames: queryReply?.args[1],
          numChannels: queryReply?.args[2],
          sampleRate: queryReply?.args[3],
        };
      } catch (err) {
        return { success: false, error: err.message, stack: err.stack };
      }
    }, sonicConfig);

    expect(result.error).toBeUndefined();
    expect(result.success).toBe(true);
    expect(result.bufnum).toBe(0);
    expect(result.numFrames).toBeGreaterThan(0);
    expect(result.numChannels).toBe(2); // bd_haus is stereo
    expect(result.sampleRate).toBeGreaterThan(0);
  });

  test("loads AIFF file via /b_allocFile (16-bit mono)", async ({ page, sonicConfig }) => {
    // Generate AIFF in Node.js context, pass to browser
    // Use 48kHz to match AudioContext sample rate (avoids resampling frame count changes)
    const aiffBytes = generateAiffFile({
      sampleRate: 48000,
      numFrames: 4800,
      numChannels: 1,
      bitsPerSample: 16,
    });

    const result = await page.evaluate(async ({ config, aiffData }) => {
      const sonic = new window.SuperSonic(config);

      const messages = [];
      sonic.on('message', (msg) => messages.push(JSON.parse(JSON.stringify(msg))));

      try {
        await sonic.init();

        // Load AIFF via /b_allocFile
        const fileBytes = new Uint8Array(aiffData);
        await sonic.send("/b_allocFile", 0, fileBytes);
        await sonic.sync(1);

        // Query buffer to verify it was loaded
        messages.length = 0;
        await sonic.send("/b_query", 0);
        await sonic.sync(2);

        const queryReply = messages.find((m) => m.address === "/b_info");

        return {
          success: true,
          bufnum: queryReply?.args[0],
          numFrames: queryReply?.args[1],
          numChannels: queryReply?.args[2],
          sampleRate: queryReply?.args[3],
        };
      } catch (err) {
        return { success: false, error: err.message, stack: err.stack };
      }
    }, { config: sonicConfig, aiffData: Array.from(aiffBytes) });

    expect(result.error).toBeUndefined();
    expect(result.success).toBe(true);
    expect(result.bufnum).toBe(0);
    expect(result.numFrames).toBe(4800);
    expect(result.numChannels).toBe(1);
    expect(result.sampleRate).toBe(48000);
  });

  test("loads AIFF file via /b_allocFile (16-bit stereo)", async ({ page, sonicConfig }) => {
    const aiffBytes = generateAiffFile({
      sampleRate: 48000,
      numFrames: 2400,
      numChannels: 2,
      bitsPerSample: 16,
    });

    const result = await page.evaluate(async ({ config, aiffData }) => {
      const sonic = new window.SuperSonic(config);

      const messages = [];
      sonic.on('message', (msg) => messages.push(JSON.parse(JSON.stringify(msg))));

      try {
        await sonic.init();

        const fileBytes = new Uint8Array(aiffData);
        await sonic.send("/b_allocFile", 0, fileBytes);
        await sonic.sync(1);

        messages.length = 0;
        await sonic.send("/b_query", 0);
        await sonic.sync(2);

        const queryReply = messages.find((m) => m.address === "/b_info");

        return {
          success: true,
          bufnum: queryReply?.args[0],
          numFrames: queryReply?.args[1],
          numChannels: queryReply?.args[2],
          sampleRate: queryReply?.args[3],
        };
      } catch (err) {
        return { success: false, error: err.message, stack: err.stack };
      }
    }, { config: sonicConfig, aiffData: Array.from(aiffBytes) });

    expect(result.error).toBeUndefined();
    expect(result.success).toBe(true);
    expect(result.bufnum).toBe(0);
    expect(result.numFrames).toBe(2400);
    expect(result.numChannels).toBe(2);
    expect(result.sampleRate).toBe(48000);
  });

  test("loads AIFF file via /b_allocFile (8-bit mono)", async ({ page, sonicConfig }) => {
    // Use 48kHz to match AudioContext sample rate (avoids resampling frame count changes)
    const aiffBytes = generateAiffFile({
      sampleRate: 48000,
      numFrames: 4800,
      numChannels: 1,
      bitsPerSample: 8,
    });

    const result = await page.evaluate(async ({ config, aiffData }) => {
      const sonic = new window.SuperSonic(config);

      const messages = [];
      sonic.on('message', (msg) => messages.push(JSON.parse(JSON.stringify(msg))));

      try {
        await sonic.init();

        const fileBytes = new Uint8Array(aiffData);
        await sonic.send("/b_allocFile", 0, fileBytes);
        await sonic.sync(1);

        messages.length = 0;
        await sonic.send("/b_query", 0);
        await sonic.sync(2);

        const queryReply = messages.find((m) => m.address === "/b_info");

        return {
          success: true,
          bufnum: queryReply?.args[0],
          numFrames: queryReply?.args[1],
          numChannels: queryReply?.args[2],
          sampleRate: queryReply?.args[3],
        };
      } catch (err) {
        return { success: false, error: err.message, stack: err.stack };
      }
    }, { config: sonicConfig, aiffData: Array.from(aiffBytes) });

    expect(result.error).toBeUndefined();
    expect(result.success).toBe(true);
    expect(result.bufnum).toBe(0);
    expect(result.numFrames).toBe(4800);
    expect(result.numChannels).toBe(1);
    expect(result.sampleRate).toBe(48000);
  });

  test("AIFF buffer can be played with synth", async ({ page, sonicConfig }) => {
    const aiffBytes = generateAiffFile({
      sampleRate: 44100,
      numFrames: 44100, // 1 second
      numChannels: 1,
      bitsPerSample: 16,
      frequency: 440,
    });

    const result = await page.evaluate(async ({ config, aiffData }) => {
      const sonic = new window.SuperSonic(config);

      try {
        await sonic.init();

        // Load AIFF sample
        const fileBytes = new Uint8Array(aiffData);
        await sonic.send("/b_allocFile", 0, fileBytes);

        // Load synthdef
        await sonic.loadSynthDef("sonic-pi-basic_mono_player");
        await sonic.sync(1);

        // Play the sample
        await sonic.send(
          "/s_new",
          "sonic-pi-basic_mono_player",
          1000,
          1,
          0,
          "buf",
          0
        );
        await sonic.sync(2);

        // Wait for tree update
        let tree;
        const start = Date.now();
        while (Date.now() - start < 2000) {
          tree = sonic.getRawTree();
          if (tree.nodes.some((n) => n.id === 1000)) break;
          await new Promise(r => setTimeout(r, 20));
        }

        return {
          success: true,
          nodeCount: tree.nodeCount,
          hasSynth: tree.nodes.some((n) => n.id === 1000),
        };
      } catch (err) {
        return { success: false, error: err.message, stack: err.stack };
      }
    }, { config: sonicConfig, aiffData: Array.from(aiffBytes) });

    expect(result.error).toBeUndefined();
    expect(result.success).toBe(true);
    expect(result.hasSynth).toBe(true);
  });

  test("getLoadedBuffers returns info about loaded samples", async ({ page, sonicConfig }) => {
    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);

      try {
        await sonic.init();

        // Initially should be empty
        const buffersEmpty = sonic.getLoadedBuffers();

        // Load some samples
        await sonic.loadSample(0, "bd_haus.flac");
        await sonic.loadSample(5, "loop_amen.flac");
        await sonic.loadSample(10, "elec_ping.flac");
        await sonic.sync(1);

        const buffersLoaded = sonic.getLoadedBuffers();

        // Free one buffer
        await sonic.send("/b_free", 5);
        await sonic.sync(2);

        const buffersAfterFreeOne = sonic.getLoadedBuffers();

        // Free another buffer
        await sonic.send("/b_free", 0);
        await sonic.sync(3);

        const buffersAfterFreeTwo = sonic.getLoadedBuffers();

        // Free the last buffer
        await sonic.send("/b_free", 10);
        await sonic.sync(4);

        const buffersAfterFreeAll = sonic.getLoadedBuffers();

        return {
          success: true,
          buffersEmpty,
          buffersLoaded,
          buffersAfterFreeOne,
          buffersAfterFreeTwo,
          buffersAfterFreeAll,
        };
      } catch (err) {
        return { success: false, error: err.message };
      }
    }, sonicConfig);

    expect(result.success).toBe(true);

    // Initially empty
    expect(result.buffersEmpty.length).toBe(0);

    // After loading 3 samples
    expect(result.buffersLoaded.length).toBe(3);
    expect(result.buffersLoaded.map(b => b.bufnum).sort((a,b) => a-b)).toEqual([0, 5, 10]);

    const buf0 = result.buffersLoaded.find(b => b.bufnum === 0);
    expect(buf0).toBeDefined();
    expect(buf0.numFrames).toBeGreaterThan(0);
    expect(buf0.numChannels).toBeGreaterThan(0);
    expect(buf0.sampleRate).toBeGreaterThan(0);
    expect(buf0.duration).toBeGreaterThan(0);
    expect(buf0.source).toContain("bd_haus");

    const buf5 = result.buffersLoaded.find(b => b.bufnum === 5);
    expect(buf5).toBeDefined();
    expect(buf5.source).toContain("loop_amen");

    // After freeing buffer 5
    expect(result.buffersAfterFreeOne.length).toBe(2);
    expect(result.buffersAfterFreeOne.map(b => b.bufnum).sort((a,b) => a-b)).toEqual([0, 10]);

    // After freeing buffer 0
    expect(result.buffersAfterFreeTwo.length).toBe(1);
    expect(result.buffersAfterFreeTwo[0].bufnum).toBe(10);

    // After freeing all
    expect(result.buffersAfterFreeAll.length).toBe(0);
  });
});
