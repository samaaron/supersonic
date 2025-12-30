import { test, expect } from "./fixtures.mjs";

/**
 * Ring Buffer Wrap-around Test
 *
 * This test specifically targets the bug where OSC messages crossing the
 * ring buffer boundary get corrupted in postMessage mode.
 *
 * The bug manifests when:
 * 1. The write head is near the end of the ring buffer
 * 2. A new message is written that would cross the boundary
 * 3. Without proper wrap-around handling, bytes are written past the buffer end
 *    instead of wrapping to the beginning, corrupting the data
 *
 * Symptom: SynthDef names like "sonic-pi-basic_stereo_player" becoming
 *          "sonic-pi-basic_stereﾭ�" (garbage after partial name)
 *
 * This test runs in BOTH postMessage and SAB modes to verify:
 * - SAB mode (reference): Uses ring_buffer_writer.js which handles wrap correctly
 * - postMessage mode: Uses drainOscQueue() which had the bug
 */

test.describe("Ring Buffer Wrap-around", () => {
  test("synthdef names remain intact when messages cross buffer boundary", async ({ page, sonicConfig, sonicMode }) => {
    console.log(`Running wrap-around test in ${sonicMode} mode`);

    const errors = [];
    const corruptedNames = [];

    page.on("console", (msg) => {
      const text = msg.text();
      // Look for synthdef errors - these would show corrupted names
      if (text.includes("SynthDef") && text.includes("not found")) {
        corruptedNames.push(text);
      }
      if (text.includes("Command not found:") ||
          text.includes("Invalid magic") ||
          text.includes("FAILURE IN SERVER")) {
        errors.push(text);
      }
    });

    await page.goto("/test/harness.html");
    await page.waitForFunction(() => window.supersonicReady === true, {
      timeout: 10000,
    });

    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);

      const debugMessages = [];
      const receivedOsc = [];

      sonic.on('debug', (msg) => {
        debugMessages.push(msg);
      });

      // Track all received OSC messages to detect any corruption
      sonic.on('osc', (msg) => {
        receivedOsc.push(msg);
      });

      try {
        await sonic.init();

        // Load a synthdef with a distinctive long name that we'll watch for corruption
        await sonic.loadSynthDefs(["sonic-pi-basic_stereo_player"]);
        await sonic.sync(1);

        // Strategy: Send many small messages to advance the write head
        // near the buffer boundary, then send messages with long synthdef
        // names that will cross the boundary.

        // The IN_BUFFER_SIZE is typically around 16KB-64KB.
        // We need to fill it enough to trigger wrap-around.

        const FILL_ROUNDS = 50;  // Number of fill rounds
        const MSGS_PER_ROUND = 100;  // Messages per round
        const TEST_NODE_BASE = 50000;

        let nodeId = TEST_NODE_BASE;
        let wrappedMessages = 0;
        let successfulCreates = 0;

        // Fill rounds - each round sends enough messages to rotate through buffer
        for (let round = 0; round < FILL_ROUNDS; round++) {
          const promises = [];

          for (let i = 0; i < MSGS_PER_ROUND; i++) {
            // Use the full long synthdef name - this creates larger messages
            // that are more likely to span the boundary
            promises.push(
              sonic.send("/s_new", "sonic-pi-basic_stereo_player", nodeId, 0, 0,
                "out", 0, "buf", 0, "rate", 1.0, "start_pos", 0.0,
                "amp", 0.0,  // Silent
                "attack", 0.001, "sustain", 0.001, "decay", 0.001, "release", 0.001)
            );
            nodeId++;
          }

          await Promise.all(promises);

          // Small delay to let processing catch up
          await new Promise(r => setTimeout(r, 10));
        }

        // Wait for all messages to process
        await new Promise(r => setTimeout(r, 500));

        // Now check if any synthdef errors occurred by looking at debug messages
        const synthdefErrors = debugMessages.filter(msg =>
          msg.text?.includes("SynthDef") && msg.text?.includes("not found")
        );

        // Extract corrupted names from errors
        const corruptedNamesFound = [];
        for (const err of synthdefErrors) {
          const match = err.text?.match(/SynthDef\s+([^\s]+)\s+not found/);
          if (match && match[1] !== "sonic-pi-basic_stereo_player") {
            corruptedNamesFound.push(match[1]);
          }
        }

        // Clean up - free all created nodes
        for (let id = TEST_NODE_BASE; id < nodeId; id++) {
          sonic.send("/n_free", id);
        }

        await new Promise(r => setTimeout(r, 100));

        return {
          success: true,
          totalMessages: (nodeId - TEST_NODE_BASE),
          synthdefErrors: synthdefErrors.length,
          corruptedNamesFound,
          debugSample: debugMessages.slice(-20),
        };
      } catch (err) {
        return { success: false, error: err.message, stack: err.stack };
      }
    }, sonicConfig);

    console.log(`Total messages sent: ${result.totalMessages}`);
    console.log(`SynthDef errors: ${result.synthdefErrors}`);

    if (result.corruptedNamesFound?.length > 0) {
      console.log("CORRUPTED SYNTHDEF NAMES DETECTED:");
      result.corruptedNamesFound.forEach(name => console.log(`  - "${name}"`));
    }

    if (corruptedNames.length > 0) {
      console.log("Console corruption errors:");
      corruptedNames.forEach(e => console.log(`  - ${e}`));
    }

    expect(result.success).toBe(true);
    // The key assertion: no corrupted synthdef names
    expect(result.corruptedNamesFound?.length || 0).toBe(0);
    expect(result.synthdefErrors).toBe(0);
  });

  test("large messages crossing boundary preserve content integrity", async ({ page, sonicConfig, sonicMode }) => {
    console.log(`Running large message wrap test in ${sonicMode} mode`);

    await page.goto("/test/harness.html");
    await page.waitForFunction(() => window.supersonicReady === true, {
      timeout: 10000,
    });

    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);

      const debugMessages = [];
      sonic.on('debug', (msg) => debugMessages.push(msg));

      try {
        await sonic.init();
        await sonic.loadSynthDefs(["sonic-pi-beep"]);
        await sonic.sync(1);

        // Strategy: Use control messages with many parameters to create
        // larger OSC messages that are more likely to span boundaries.
        //
        // We'll use /n_setn which sets multiple controls at once,
        // creating larger messages.

        // First create a synth
        await sonic.send("/s_new", "sonic-pi-beep", 99999, 0, 0,
          "note", 60, "amp", 0.0, "sustain", 100);

        // Now spam large n_set messages to fill the buffer
        const ITERATIONS = 200;
        let messagesSent = 0;

        for (let iter = 0; iter < ITERATIONS; iter++) {
          // Send a batch of messages with various sizes
          const promises = [];

          for (let i = 0; i < 50; i++) {
            // Create messages of varying sizes by using different numbers of parameters
            const params = ["amp", Math.random() * 0.01];
            // Add extra dummy params to increase message size variably
            for (let p = 0; p < (i % 10); p++) {
              params.push("note", 60 + Math.random());
            }

            promises.push(sonic.send("/n_set", 99999, ...params));
            messagesSent++;
          }

          await Promise.all(promises);
        }

        // Wait for processing
        await new Promise(r => setTimeout(r, 300));

        // Check for corruption indicators
        const errors = debugMessages.filter(msg =>
          msg.text?.includes("Command not found:") ||
          msg.text?.includes("Invalid magic") ||
          msg.text?.includes("FAILURE IN SERVER") ||
          msg.text?.includes("OSC parse error")
        );

        // Clean up
        await sonic.send("/n_free", 99999);

        return {
          success: true,
          messagesSent,
          errorsFound: errors.length,
          errorSamples: errors.slice(0, 5).map(e => e.text),
        };
      } catch (err) {
        return { success: false, error: err.message };
      }
    }, sonicConfig);

    console.log(`Messages sent: ${result.messagesSent}`);

    if (result.errorsFound > 0) {
      console.log("ERRORS DETECTED:");
      result.errorSamples.forEach(e => console.log(`  - ${e}`));
    }

    expect(result.success).toBe(true);
    expect(result.errorsFound).toBe(0);
  });

  test("continuous message stream maintains integrity over time", async ({ page, sonicConfig, sonicMode }) => {
    // This test simulates the real-world scenario where the bug was discovered:
    // continuous message streaming over an extended period, causing the buffer
    // to wrap multiple times.

    console.log(`Running continuous stream test in ${sonicMode} mode`);

    const corruptionErrors = [];

    page.on("console", (msg) => {
      const text = msg.text();
      if (text.includes("SynthDef") && text.includes("not found") &&
          !text.includes("sonic-pi-basic_stereo_player")) {
        // Only flag as corruption if it's NOT the expected synthdef name
        corruptionErrors.push(text);
      }
      if (text.includes("Command not found:")) {
        corruptionErrors.push(text);
      }
    });

    await page.goto("/test/harness.html");
    await page.waitForFunction(() => window.supersonicReady === true, {
      timeout: 10000,
    });

    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);

      const debugMessages = [];
      sonic.on('debug', (msg) => debugMessages.push(msg));

      try {
        await sonic.init();
        await sonic.loadSynthDefs(["sonic-pi-basic_stereo_player"]);
        await sonic.sync(1);

        // Run continuous stream for 10 seconds
        // This should cause the ring buffer to wrap multiple times
        const TEST_DURATION_MS = 10000;
        const startTime = performance.now();
        let messageCount = 0;
        let nodeId = 60000;

        // Continuous send loop
        while (performance.now() - startTime < TEST_DURATION_MS) {
          // Send batch of messages
          const batch = [];
          for (let i = 0; i < 20; i++) {
            batch.push(
              sonic.send("/s_new", "sonic-pi-basic_stereo_player", nodeId++, 0, 0,
                "out", 0, "buf", 0, "rate", 1.0, "amp", 0.0,
                "attack", 0.001, "decay", 0.001, "sustain", 0.001, "release", 0.001)
            );
            messageCount++;
          }

          await Promise.all(batch);

          // Tiny delay to prevent completely overwhelming
          await new Promise(r => setTimeout(r, 1));
        }

        await new Promise(r => setTimeout(r, 500));

        // Check for corrupted synthdef names
        const corruptedNames = debugMessages
          .filter(msg => msg.text?.includes("SynthDef") && msg.text?.includes("not found"))
          .filter(msg => !msg.text?.includes("sonic-pi-basic_stereo_player"));

        return {
          success: true,
          messageCount,
          duration: performance.now() - startTime,
          corruptedCount: corruptedNames.length,
          corruptedSamples: corruptedNames.slice(0, 5).map(m => m.text),
        };
      } catch (err) {
        return { success: false, error: err.message };
      }
    }, sonicConfig);

    console.log(`Sent ${result.messageCount} messages over ${Math.round(result.duration)}ms`);
    console.log(`Rate: ${Math.round(result.messageCount / (result.duration / 1000))} msgs/sec`);

    if (result.corruptedCount > 0) {
      console.log("CORRUPTION DETECTED:");
      result.corruptedSamples.forEach(s => console.log(`  - ${s}`));
    }

    expect(result.success).toBe(true);
    expect(result.corruptedCount).toBe(0);
    expect(corruptionErrors.length).toBe(0);
  });
});
