/**
 * Reply Channel Tests
 *
 * Tests for per-OscChannel reply delivery — the ability for workers and
 * AudioWorklets to receive OSC replies directly without going through
 * the main thread.
 *
 * Phase 2: SAB-level fan-out verification
 * Phase 3+: OscChannel API tests added incrementally
 */

import { test, expect } from "./fixtures.mjs";

// =============================================================================
// SAB REPLY BUFFER FAN-OUT
// =============================================================================

test.describe("Reply channel SAB fan-out", () => {
  test("reply appears in channel buffer when active flag is set", async ({ page, sonicConfig, sonicMode }) => {
    test.skip(sonicMode !== 'sab', 'SAB-only test');
    await page.goto("/test/harness.html");

    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      await sonic.init();

      // Get the SAB and layout constants
      const sab = sonic.sharedBuffer;
      const layout = sonic.bufferConstants;
      if (!layout.REPLY_CHANNEL_COUNT) return { skip: true };

      const atomicView = new Int32Array(sab);
      const uint8View = new Uint8Array(sab);
      const base = sonic.ringBufferBase;

      // Compute channel 0 control indices
      const controlBase = base + layout.REPLY_CHANNELS_CONTROL_START;
      const headIndex = controlBase / 4;
      const tailIndex = (controlBase + 4) / 4;
      const activeIndex = (controlBase + 8) / 4;

      // Activate channel 0
      Atomics.store(atomicView, activeIndex, 1);

      // Send a command that produces a reply
      await sonic.send("/status");
      await sonic.sync(1);

      // Give the fan-out a moment
      await new Promise(r => setTimeout(r, 200));

      // Read channel 0's head — should have advanced
      const head = Atomics.load(atomicView, headIndex);
      const tail = Atomics.load(atomicView, tailIndex);

      // Deactivate
      Atomics.store(atomicView, activeIndex, 0);
      await sonic.shutdown();

      return { head, tail, headAdvanced: head !== tail };
    }, sonicConfig);

    if (result.skip) test.skip(true, 'WASM lacks reply channel support');
    expect(result.headAdvanced).toBe(true);
  });

  test("inactive channel receives nothing", async ({ page, sonicConfig, sonicMode }) => {
    test.skip(sonicMode !== 'sab', 'SAB-only test');
    await page.goto("/test/harness.html");

    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      await sonic.init();

      const sab = sonic.sharedBuffer;
      const layout = sonic.bufferConstants;
      if (!layout.REPLY_CHANNEL_COUNT) return { skip: true };

      const atomicView = new Int32Array(sab);
      const base = sonic.ringBufferBase;

      // Channel 0 stays inactive (active = 0, the default)
      const controlBase = base + layout.REPLY_CHANNELS_CONTROL_START;
      const headIndex = controlBase / 4;
      const tailIndex = (controlBase + 4) / 4;

      // Send commands that produce replies
      await sonic.send("/status");
      await sonic.sync(1);
      await new Promise(r => setTimeout(r, 200));

      const head = Atomics.load(atomicView, headIndex);
      const tail = Atomics.load(atomicView, tailIndex);

      await sonic.shutdown();
      return { head, tail, empty: head === tail };
    }, sonicConfig);

    if (result.skip) test.skip(true, 'WASM lacks reply channel support');
    expect(result.empty).toBe(true);
  });

  test("main thread still receives replies when channel is active", async ({ page, sonicConfig, sonicMode }) => {
    test.skip(sonicMode !== 'sab', 'SAB-only test');
    await page.goto("/test/harness.html");

    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      const messages = [];
      sonic.on('in', (msg) => messages.push(msg));
      await sonic.init();

      const sab = sonic.sharedBuffer;
      const layout = sonic.bufferConstants;
      if (!layout.REPLY_CHANNEL_COUNT) return { skip: true };

      const atomicView = new Int32Array(sab);
      const base = sonic.ringBufferBase;

      // Activate channel 0
      const controlBase = base + layout.REPLY_CHANNELS_CONTROL_START;
      const activeIndex = (controlBase + 8) / 4;
      Atomics.store(atomicView, activeIndex, 1);

      // Send command and wait for reply
      await sonic.send("/status");
      await sonic.sync(1);
      await new Promise(r => setTimeout(r, 200));

      Atomics.store(atomicView, activeIndex, 0);

      const statusReply = messages.find(m => m[0] === "/status.reply");
      await sonic.shutdown();

      return { gotReply: !!statusReply };
    }, sonicConfig);

    if (result.skip) test.skip(true, 'WASM lacks reply channel support');
    expect(result.gotReply).toBe(true);
  });

  test("reply data in channel buffer is valid OSC", async ({ page, sonicConfig, sonicMode }) => {
    test.skip(sonicMode !== 'sab', 'SAB-only test');
    await page.goto("/test/harness.html");

    const result = await page.evaluate(async (config) => {
      const { readMessagesFromBuffer } = await import("/js/lib/ring_buffer_core.js");

      const sonic = new window.SuperSonic(config);
      await sonic.init();

      const sab = sonic.sharedBuffer;
      const layout = sonic.bufferConstants;
      if (!layout.REPLY_CHANNEL_COUNT) return { skip: true };

      const atomicView = new Int32Array(sab);
      const uint8View = new Uint8Array(sab);
      const dataView = new DataView(sab);
      const base = sonic.ringBufferBase;

      // Activate channel 0
      const controlBase = base + layout.REPLY_CHANNELS_CONTROL_START;
      const headIndex = controlBase / 4;
      const tailIndex = (controlBase + 4) / 4;
      const activeIndex = (controlBase + 8) / 4;
      Atomics.store(atomicView, activeIndex, 1);

      // Send /status which produces /status.reply
      await sonic.send("/status");
      await sonic.sync(1);
      await new Promise(r => setTimeout(r, 200));

      const head = Atomics.load(atomicView, headIndex);
      const tail = Atomics.load(atomicView, tailIndex);

      // Read messages from channel 0's reply buffer
      const bufferStart = base + layout.REPLY_CHANNELS_BUFFER_START;
      const bufferSize = layout.REPLY_CHANNEL_BUFFER_SIZE;
      const addresses = [];

      readMessagesFromBuffer({
        uint8View, dataView,
        bufferStart, bufferSize,
        head, tail,
        messageMagic: layout.MESSAGE_MAGIC,
        paddingMagic: layout.PADDING_MAGIC,
        headerSize: 16,
        maxMessages: 100,
        onMessage: (payloadOffset, payloadLength, sequence, sourceId) => {
          // Read the OSC address from the payload
          const bytes = uint8View.slice(payloadOffset, payloadOffset + Math.min(payloadLength, 64));
          let end = 0;
          while (end < bytes.length && bytes[end] !== 0) end++;
          const addr = new TextDecoder().decode(bytes.subarray(0, end));
          addresses.push(addr);
        },
      });

      Atomics.store(atomicView, activeIndex, 0);
      await sonic.shutdown();

      return { addresses, hasStatusReply: addresses.includes("/status.reply") };
    }, sonicConfig);

    if (result.skip) test.skip(true, 'WASM lacks reply channel support');
    expect(result.hasStatusReply).toBe(true);
  });

  test("multiple active channels all receive the same reply", async ({ page, sonicConfig, sonicMode }) => {
    test.skip(sonicMode !== 'sab', 'SAB-only test');
    await page.goto("/test/harness.html");

    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      await sonic.init();

      const sab = sonic.sharedBuffer;
      const layout = sonic.bufferConstants;
      if (!layout.REPLY_CHANNEL_COUNT) return { skip: true };

      const atomicView = new Int32Array(sab);
      const base = sonic.ringBufferBase;

      // Activate channels 0, 1, 2
      const channelHeads = [];
      for (let i = 0; i < 3; i++) {
        const controlBase = base + layout.REPLY_CHANNELS_CONTROL_START + (i * layout.REPLY_CHANNEL_CONTROL_SIZE);
        const activeIndex = (controlBase + 8) / 4;
        Atomics.store(atomicView, activeIndex, 1);
      }

      await sonic.send("/status");
      await sonic.sync(1);
      await new Promise(r => setTimeout(r, 200));

      // Check all three channels advanced
      for (let i = 0; i < 3; i++) {
        const controlBase = base + layout.REPLY_CHANNELS_CONTROL_START + (i * layout.REPLY_CHANNEL_CONTROL_SIZE);
        const headIndex = controlBase / 4;
        const tailIndex = (controlBase + 4) / 4;
        const activeIndex = (controlBase + 8) / 4;
        const head = Atomics.load(atomicView, headIndex);
        const tail = Atomics.load(atomicView, tailIndex);
        channelHeads.push({ channel: i, advanced: head !== tail });
        Atomics.store(atomicView, activeIndex, 0);
      }

      await sonic.shutdown();
      return { channelHeads };
    }, sonicConfig);

    if (result.skip) test.skip(true, 'WASM lacks reply channel support');
    for (const ch of result.channelHeads) {
      expect(ch.advanced).toBe(true);
    }
  });
});

// =============================================================================
// PM REPLY PORT FAN-OUT
// =============================================================================

test.describe("Reply channel PM fan-out", () => {
  test("reply arrives on registered reply port", async ({ page, sonicConfig, sonicMode }) => {
    test.skip(sonicMode !== 'postMessage', 'PM-only test');
    await page.goto("/test/harness.html");

    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      await sonic.init();

      // Create a MessageChannel — send one end to the worklet as a reply port
      const channel = new MessageChannel();

      // Access the worklet port via the transport's internal worklet port.
      // We need to send addReplyPort to the AudioWorkletNode.
      // The AudioWorkletNode is accessible via sonic internals.
      const workletNode = sonic.node?.input;
      if (!workletNode) return { skip: true, reason: 'no workletNode' };

      workletNode.port.postMessage(
        { type: 'addReplyPort', sourceId: 99 },
        [channel.port1]
      );

      // Collect replies on port2
      const replies = [];
      channel.port2.onmessage = (e) => {
        if (e.data.type === 'oscReplies') {
          replies.push(e.data);
        }
      };

      // Send /status and wait for replies
      await sonic.send("/status");
      await sonic.sync(1);
      await new Promise(r => setTimeout(r, 300));

      // Unregister
      workletNode.port.postMessage({ type: 'removeReplyPort', sourceId: 99 });
      channel.port2.close();

      await sonic.shutdown();
      return { replyCount: replies.length, gotReplies: replies.length > 0 };
    }, sonicConfig);

    if (result.skip) test.skip(true, result.reason);
    expect(result.gotReplies).toBe(true);
  });

  test("unregistered port receives no more replies", async ({ page, sonicConfig, sonicMode }) => {
    test.skip(sonicMode !== 'postMessage', 'PM-only test');
    await page.goto("/test/harness.html");

    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      await sonic.init();

      const channel = new MessageChannel();
      const workletNode = sonic.node?.input;
      if (!workletNode) return { skip: true };

      workletNode.port.postMessage(
        { type: 'addReplyPort', sourceId: 99 },
        [channel.port1]
      );

      const replies = [];
      channel.port2.onmessage = (e) => {
        if (e.data.type === 'oscReplies') replies.push(e.data);
      };

      // Send first command, verify reply
      await sonic.send("/status");
      await sonic.sync(1);
      await new Promise(r => setTimeout(r, 200));
      const countBefore = replies.length;

      // Unregister
      workletNode.port.postMessage({ type: 'removeReplyPort', sourceId: 99 });
      await new Promise(r => setTimeout(r, 100));

      // Send second command — should NOT arrive on the port
      await sonic.send("/status");
      await sonic.sync(2);
      await new Promise(r => setTimeout(r, 200));
      const countAfter = replies.length;

      channel.port2.close();
      await sonic.shutdown();
      return { countBefore, countAfter, noNewReplies: countAfter === countBefore };
    }, sonicConfig);

    if (result.skip) test.skip(true, 'no workletNode');
    expect(result.countBefore).toBeGreaterThan(0);
    expect(result.noNewReplies).toBe(true);
  });

  test("main thread still receives replies when reply port is registered", async ({ page, sonicConfig, sonicMode }) => {
    test.skip(sonicMode !== 'postMessage', 'PM-only test');
    await page.goto("/test/harness.html");

    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      const messages = [];
      sonic.on('in', (msg) => messages.push(msg));
      await sonic.init();

      const channel = new MessageChannel();
      const workletNode = sonic.node?.input;
      if (!workletNode) return { skip: true };

      workletNode.port.postMessage(
        { type: 'addReplyPort', sourceId: 99 },
        [channel.port1]
      );

      // Discard port2 replies — we only care about main thread
      channel.port2.onmessage = () => {};

      await sonic.send("/status");
      await sonic.sync(1);
      await new Promise(r => setTimeout(r, 200));

      const statusReply = messages.find(m => m[0] === "/status.reply");

      workletNode.port.postMessage({ type: 'removeReplyPort', sourceId: 99 });
      channel.port2.close();
      await sonic.shutdown();

      return { gotReply: !!statusReply };
    }, sonicConfig);

    if (result.skip) test.skip(true, 'no workletNode');
    expect(result.gotReply).toBe(true);
  });
});

// =============================================================================
// OscChannel onReply() / offReply() / pollReplies() API
// =============================================================================

test.describe("OscChannel reply API", () => {
  test("onReply receives /status.reply on main thread OscChannel", async ({ page, sonicConfig }) => {
    await page.goto("/test/harness.html");

    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      await sonic.init();

      const channel = sonic.createOscChannel();
      const replies = [];

      channel.onReply((oscData, sequence) => {
        // Decode the address from raw OSC bytes
        let end = 0;
        while (end < oscData.length && oscData[end] !== 0) end++;
        const addr = new TextDecoder().decode(oscData.subarray(0, end));
        replies.push({ addr, sequence });
      });

      await sonic.send("/status");
      await sonic.sync(1);
      await new Promise(r => setTimeout(r, 500));

      channel.offReply();
      channel.close();
      await sonic.shutdown();

      const hasStatusReply = replies.some(r => r.addr === "/status.reply");
      return { replyCount: replies.length, hasStatusReply };
    }, sonicConfig);

    expect(result.hasStatusReply).toBe(true);
  });

  test("offReply stops delivery", async ({ page, sonicConfig }) => {
    await page.goto("/test/harness.html");

    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      await sonic.init();

      const channel = sonic.createOscChannel();
      const replies = [];

      channel.onReply((oscData) => {
        let end = 0;
        while (end < oscData.length && oscData[end] !== 0) end++;
        replies.push(new TextDecoder().decode(oscData.subarray(0, end)));
      });

      await sonic.send("/status");
      await sonic.sync(1);
      await new Promise(r => setTimeout(r, 300));
      const countBefore = replies.length;

      channel.offReply();
      await new Promise(r => setTimeout(r, 100));

      // Send another command — should NOT arrive
      await sonic.send("/status");
      await sonic.sync(2);
      await new Promise(r => setTimeout(r, 300));
      const countAfter = replies.length;

      channel.close();
      await sonic.shutdown();
      return { countBefore, countAfter, stopped: countAfter === countBefore };
    }, sonicConfig);

    expect(result.countBefore).toBeGreaterThan(0);
    expect(result.stopped).toBe(true);
  });

  test("two channels both receive the same reply (broadcast)", async ({ page, sonicConfig }) => {
    await page.goto("/test/harness.html");

    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      await sonic.init();

      const ch1 = sonic.createOscChannel();
      const ch2 = sonic.createOscChannel();
      const replies1 = [];
      const replies2 = [];

      const decodeAddr = (oscData) => {
        let end = 0;
        while (end < oscData.length && oscData[end] !== 0) end++;
        return new TextDecoder().decode(oscData.subarray(0, end));
      };

      ch1.onReply((oscData) => replies1.push(decodeAddr(oscData)));
      ch2.onReply((oscData) => replies2.push(decodeAddr(oscData)));

      await sonic.send("/status");
      await sonic.sync(1);
      await new Promise(r => setTimeout(r, 500));

      ch1.offReply(); ch1.close();
      ch2.offReply(); ch2.close();
      await sonic.shutdown();

      return {
        ch1HasStatus: replies1.includes("/status.reply"),
        ch2HasStatus: replies2.includes("/status.reply"),
      };
    }, sonicConfig);

    expect(result.ch1HasStatus).toBe(true);
    expect(result.ch2HasStatus).toBe(true);
  });

  test("close() cleans up reply channel", async ({ page, sonicConfig }) => {
    await page.goto("/test/harness.html");

    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      await sonic.init();

      const channel = sonic.createOscChannel();
      const replies = [];

      channel.onReply((oscData) => replies.push(true));

      await sonic.send("/status");
      await sonic.sync(1);
      await new Promise(r => setTimeout(r, 300));
      const countBefore = replies.length;

      // close() should implicitly call offReply()
      channel.close();
      await new Promise(r => setTimeout(r, 100));

      await sonic.send("/status");
      await sonic.sync(2);
      await new Promise(r => setTimeout(r, 300));
      const countAfter = replies.length;

      await sonic.shutdown();
      return { countBefore, countAfter, stopped: countAfter === countBefore };
    }, sonicConfig);

    expect(result.countBefore).toBeGreaterThan(0);
    expect(result.stopped).toBe(true);
  });

  test("9th channel onReply throws", async ({ page, sonicConfig, sonicMode }) => {
    test.skip(sonicMode !== 'sab', 'SAB slot exhaustion test');
    await page.goto("/test/harness.html");

    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      await sonic.init();

      const channels = [];
      let error = null;

      try {
        for (let i = 0; i < 9; i++) {
          const ch = sonic.createOscChannel();
          ch.onReply(() => {});
          channels.push(ch);
        }
      } catch (e) {
        error = e.message;
      }

      for (const ch of channels) ch.close();
      await sonic.shutdown();

      return { error, channelsCreated: channels.length };
    }, sonicConfig);

    expect(result.channelsCreated).toBe(8);
    expect(result.error).toContain('reply channel slots are in use');
  });

  test("worker receives replies via onReply after transfer", async ({ page, sonicConfig }) => {
    await page.goto("/test/harness.html");

    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      await sonic.init();
      await sonic.loadSynthDef("sonic-pi-beep");

      const channel = sonic.createOscChannel();

      // Create a worker that uses the channel
      const worker = new Worker("/test/assets/osc_channel_test_worker.js", { type: "module" });

      await new Promise((resolve, reject) => {
        const timeout = setTimeout(() => reject(new Error("Worker ready timeout")), 5000);
        worker.onmessage = (e) => {
          if (e.data.type === "ready") { clearTimeout(timeout); resolve(); }
        };
      });

      worker.postMessage(
        { type: "initChannel", channel: channel.transferable },
        channel.transferList
      );

      await new Promise((resolve, reject) => {
        const timeout = setTimeout(() => reject(new Error("Channel ready timeout")), 5000);
        worker.onmessage = (e) => {
          if (e.data.type === "channelReady") { clearTimeout(timeout); resolve(); }
        };
      });

      // Ask the worker to register for replies and send /status
      const workerReplies = await new Promise((resolve, reject) => {
        const timeout = setTimeout(() => reject(new Error("Reply timeout")), 5000);
        worker.onmessage = (e) => {
          if (e.data.type === "replyResults") {
            clearTimeout(timeout);
            resolve(e.data.addresses);
          }
        };
        worker.postMessage({ type: "testReplies" });
      });

      worker.terminate();
      await sonic.shutdown();
      return { workerReplies, hasStatusReply: workerReplies.includes("/status.reply") };
    }, sonicConfig);

    expect(result.hasStatusReply).toBe(true);
  });
});

// =============================================================================
// AudioWorklet-to-AudioWorklet (Sonic Tau scenario)
// =============================================================================

test.describe("AudioWorklet-to-AudioWorklet replies", () => {
  test("worklet polls /status.reply from SAB reply buffer in process()", async ({ page, sonicConfig, sonicMode }) => {
    test.skip(sonicMode !== 'sab', 'SAB reply buffer polling test');
    await page.goto("/test/harness.html");

    const result = await page.evaluate(async (config) => {
      async function setupTestWorklet(sonic) {
        const ctx = sonic.node.input.context;
        await ctx.audioWorklet.addModule("/test/assets/reply_test_worklet.js");
        const testNode = new AudioWorkletNode(ctx, "reply-test-processor");
        testNode.connect(ctx.destination);
        return testNode;
      }

      const sonic = new window.SuperSonic(config);
      await sonic.init();

      const testNode = await setupTestWorklet(sonic);
      const sab = sonic.sharedBuffer;
      const layout = sonic.bufferConstants;
      const base = sonic.ringBufferBase;

      // Pick reply slot 0 and send its SAB config to the worklet
      const controlBase = base + layout.REPLY_CHANNELS_CONTROL_START;
      const bufferStart = base + layout.REPLY_CHANNELS_BUFFER_START;

      await new Promise((resolve, reject) => {
        const timeout = setTimeout(() => reject(new Error("Ready timeout")), 5000);
        testNode.port.onmessage = (e) => {
          if (e.data.type === "ready") { clearTimeout(timeout); resolve(); }
        };
        testNode.port.postMessage({
          type: "initReplyBuffer",
          sharedBuffer: sab,
          headIdx: controlBase / 4,
          tailIdx: (controlBase + 4) / 4,
          activeIdx: (controlBase + 8) / 4,
          bufferStart,
          bufferSize: layout.REPLY_CHANNEL_BUFFER_SIZE,
          messageMagic: layout.MESSAGE_MAGIC,
        });
      });

      // Start collecting
      await new Promise((resolve, reject) => {
        const timeout = setTimeout(() => reject(new Error("Collecting timeout")), 5000);
        testNode.port.onmessage = (e) => {
          if (e.data.type === "collectingStarted") { clearTimeout(timeout); resolve(); }
        };
        testNode.port.postMessage({ type: "startCollecting" });
      });

      // Send /status from main thread
      await sonic.send("/status");
      await sonic.sync(1);
      await new Promise(r => setTimeout(r, 1000));

      // Get results
      const replies = await new Promise((resolve, reject) => {
        const timeout = setTimeout(() => reject(new Error("Results timeout")), 5000);
        testNode.port.onmessage = (e) => {
          if (e.data.type === "results") { clearTimeout(timeout); resolve(e.data.replies); }
        };
        testNode.port.postMessage({ type: "getResults" });
      });

      testNode.disconnect();
      await sonic.shutdown();

      return { replies, hasStatusReply: replies.includes("/status.reply") };
    }, sonicConfig);
    expect(result.hasStatusReply).toBe(true);
  });

  test("worklet receives replies via PM reply port", async ({ page, sonicConfig, sonicMode }) => {
    test.skip(sonicMode !== 'postMessage', 'PM reply port test');
    await page.goto("/test/harness.html");

    const result = await page.evaluate(async (config) => {
      async function setupTestWorklet(sonic) {
        const ctx = sonic.node.input.context;
        await ctx.audioWorklet.addModule("/test/assets/reply_test_worklet.js");
        const testNode = new AudioWorkletNode(ctx, "reply-test-processor");
        testNode.connect(ctx.destination);
        return testNode;
      }

      const sonic = new window.SuperSonic(config);
      await sonic.init();

      const testNode = await setupTestWorklet(sonic);
      const workletNode = sonic.node?.input;

      // Create a reply MessageChannel and register one end with the SuperSonic worklet
      const replyChannel = new MessageChannel();
      workletNode.port.postMessage(
        { type: 'addReplyPort', sourceId: 77 },
        [replyChannel.port1]
      );

      // Send the other end to the test worklet
      await new Promise((resolve, reject) => {
        const timeout = setTimeout(() => reject(new Error("Ready timeout")), 5000);
        testNode.port.onmessage = (e) => {
          if (e.data.type === "ready") { clearTimeout(timeout); resolve(); }
        };
        testNode.port.postMessage(
          { type: "initReplyPort" },
          [replyChannel.port2]
        );
      });

      // Start collecting
      await new Promise((resolve, reject) => {
        const timeout = setTimeout(() => reject(new Error("Collecting timeout")), 5000);
        testNode.port.onmessage = (e) => {
          if (e.data.type === "collectingStarted") { clearTimeout(timeout); resolve(); }
        };
        testNode.port.postMessage({ type: "startCollecting" });
      });

      // Send /status
      await sonic.send("/status");
      await sonic.sync(1);
      await new Promise(r => setTimeout(r, 1000));

      // Get results
      const replies = await new Promise((resolve, reject) => {
        const timeout = setTimeout(() => reject(new Error("Results timeout")), 5000);
        testNode.port.onmessage = (e) => {
          if (e.data.type === "results") { clearTimeout(timeout); resolve(e.data.replies); }
        };
        testNode.port.postMessage({ type: "getResults" });
      });

      workletNode.port.postMessage({ type: 'removeReplyPort', sourceId: 77 });
      testNode.disconnect();
      await sonic.shutdown();

      return { replies, hasStatusReply: replies.includes("/status.reply") };
    }, sonicConfig);

    expect(result.hasStatusReply).toBe(true);
  });

  test("main thread and worklet both receive the same reply", async ({ page, sonicConfig, sonicMode }) => {
    test.skip(sonicMode !== 'sab', 'SAB mode for simplicity');
    await page.goto("/test/harness.html");

    const result = await page.evaluate(async (config) => {
      async function setupTestWorklet(sonic) {
        const ctx = sonic.node.input.context;
        await ctx.audioWorklet.addModule("/test/assets/reply_test_worklet.js");
        const testNode = new AudioWorkletNode(ctx, "reply-test-processor");
        testNode.connect(ctx.destination);
        return testNode;
      }

      const sonic = new window.SuperSonic(config);
      const mainReplies = [];
      sonic.on('in', (msg) => mainReplies.push(msg));
      await sonic.init();

      const testNode = await setupTestWorklet(sonic);
      const sab = sonic.sharedBuffer;
      const layout = sonic.bufferConstants;
      const base = sonic.ringBufferBase;

      const controlBase = base + layout.REPLY_CHANNELS_CONTROL_START;
      const bufferStart = base + layout.REPLY_CHANNELS_BUFFER_START;

      await new Promise((resolve, reject) => {
        const timeout = setTimeout(() => reject(new Error("Ready timeout")), 5000);
        testNode.port.onmessage = (e) => {
          if (e.data.type === "ready") { clearTimeout(timeout); resolve(); }
        };
        testNode.port.postMessage({
          type: "initReplyBuffer",
          sharedBuffer: sab,
          headIdx: controlBase / 4,
          tailIdx: (controlBase + 4) / 4,
          activeIdx: (controlBase + 8) / 4,
          bufferStart,
          bufferSize: layout.REPLY_CHANNEL_BUFFER_SIZE,
          messageMagic: layout.MESSAGE_MAGIC,
        });
      });

      await new Promise((resolve, reject) => {
        const timeout = setTimeout(() => reject(new Error("Collecting timeout")), 5000);
        testNode.port.onmessage = (e) => {
          if (e.data.type === "collectingStarted") { clearTimeout(timeout); resolve(); }
        };
        testNode.port.postMessage({ type: "startCollecting" });
      });

      await sonic.send("/status");
      await sonic.sync(1);
      await new Promise(r => setTimeout(r, 1000));

      const workletReplies = await new Promise((resolve, reject) => {
        const timeout = setTimeout(() => reject(new Error("Results timeout")), 5000);
        testNode.port.onmessage = (e) => {
          if (e.data.type === "results") { clearTimeout(timeout); resolve(e.data.replies); }
        };
        testNode.port.postMessage({ type: "getResults" });
      });

      testNode.disconnect();
      await sonic.shutdown();

      const mainHasStatus = mainReplies.some(m => m[0] === "/status.reply");
      const workletHasStatus = workletReplies.includes("/status.reply");

      return { mainHasStatus, workletHasStatus };
    }, sonicConfig);

    expect(result.mainHasStatus).toBe(true);
    expect(result.workletHasStatus).toBe(true);
  });
});
