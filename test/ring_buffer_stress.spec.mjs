import { test, expect } from "@playwright/test";

/**
 * Ring Buffer Stress Test
 *
 * This test attempts to reproduce race conditions in the ring buffer
 * by rapidly sending OSC messages from multiple sources (main thread
 * direct writes and worker writes).
 *
 * Issue: https://github.com/samaaron/supersonic/issues/2
 * Symptoms: "Command not found: @␙#", garbage node IDs, "Bundle too large"
 */

test.describe("Ring Buffer Stress Test (Demo Page)", () => {
  test("spam synth pad interactions", async ({ page }) => {
    // Test using actual demo.html to replicate real user behavior
    const errors = [];

    page.on("console", (msg) => {
      const text = msg.text();
      if (text.includes("Command not found:") ||
          text.includes("Bundle too large") ||
          text.includes("Invalid magic") ||
          text.includes("FAILURE IN SERVER") ||
          (text.includes("Node -") && text.includes("not found"))) {
        errors.push(text);
        console.log("CORRUPTION:", text);
      }
    });

    // Go to actual demo page
    await page.goto("/example/demo.html");

    // Wait for page to load
    await page.waitForSelector("#init-button");

    // Click boot button
    await page.click("#init-button");

    // Wait for SuperSonic to initialize by checking debug log
    await page.waitForFunction(
      () => document.querySelector("#debug-log")?.textContent?.includes("scsynth-nrt ready"),
      { timeout: 30000 }
    );

    // Give it a moment to stabilize
    await page.waitForTimeout(1000);

    // Get the synth pad element
    const pad = await page.locator("#synth-pad");
    const box = await pad.boundingBox();

    if (!box) {
      throw new Error("Could not find synth pad");
    }

    // Rapidly move mouse across the pad for 10 seconds
    const startTime = Date.now();
    const duration = 10000;  // 10 seconds
    let moveCount = 0;

    // Start by clicking on the pad to activate it
    await page.mouse.click(box.x + box.width / 2, box.y + box.height / 2);

    // Hold mouse down and move rapidly
    await page.mouse.down();

    while (Date.now() - startTime < duration) {
      // Random position within pad
      const x = box.x + Math.random() * box.width;
      const y = box.y + Math.random() * box.height;

      // Move without any delay
      await page.mouse.move(x, y, { steps: 1 });
      moveCount++;

      // Occasionally release and re-click to simulate taps
      if (moveCount % 50 === 0) {
        await page.mouse.up();
        await page.waitForTimeout(10);
        await page.mouse.down();
      }
    }

    await page.mouse.up();

    console.log(`Performed ${moveCount} mouse movements in ${duration}ms`);

    // Wait for any queued messages to process
    await page.waitForTimeout(1000);

    if (errors.length > 0) {
      console.log("CORRUPTION ERRORS FOUND:");
      errors.forEach(e => console.log(`  ${e}`));
    }

    expect(errors.length).toBe(0);
  });
});

test.describe("Ring Buffer Stress Test (Harness)", () => {
  test("rapid OSC messages should not cause corruption", async ({ page }) => {
    // Collect all console output for analysis
    const consoleMessages = [];
    const errors = [];

    page.on("console", (msg) => {
      const text = msg.text();
      consoleMessages.push({ type: msg.type(), text });

      // Look for corruption indicators
      if (text.includes("Command not found:") ||
          text.includes("Bundle too large") ||
          text.includes("Invalid magic") ||
          text.includes("Node -") ||  // Negative node IDs are garbage
          text.includes("FAILURE IN SERVER")) {
        errors.push(text);
      }
    });

    page.on("pageerror", (err) => {
      errors.push(`Page error: ${err.message}`);
    });

    await page.goto("/test/harness.html");

    // Wait for SuperSonic module to load
    await page.waitForFunction(() => window.supersonicReady === true, {
      timeout: 10000,
    });

    // Run the stress test
    const result = await page.evaluate(async () => {
      const sonic = new window.SuperSonic({
        workerBaseURL: "/dist/workers/",
        wasmBaseURL: "/dist/wasm/",
        sampleBaseURL: "/dist/samples/",
        synthdefBaseURL: "/dist/synthdefs/",
      });

      const debugMessages = [];
      sonic.onDebug = (msg) => {
        debugMessages.push(msg);
      };

      try {
        await sonic.init();
        await sonic.loadSynthDefs(["sonic-pi-beep"]);
        await sonic.sync(1);

        // Stress test: send rapid OSC messages
        // This simulates rapid mouse movements on the synth pad
        // More aggressive parameters to try to trigger race conditions
        const NUM_MESSAGES = 5000;
        const BURST_SIZE = 200;  // Messages per burst - higher to stress buffer
        const BURST_DELAY = 1;   // ms between bursts - minimal delay

        let messagesSent = 0;

        for (let burst = 0; burst < NUM_MESSAGES / BURST_SIZE; burst++) {
          // Send a burst of messages rapidly (no await between them)
          const promises = [];
          for (let i = 0; i < BURST_SIZE; i++) {
            const nodeId = 10000 + messagesSent;
            // Alternate between different message types to stress different paths
            if (i % 3 === 0) {
              // s_new - create synth (goes through prescheduler for timing)
              promises.push(
                sonic.send("/s_new", "sonic-pi-beep", nodeId, 0, 0,
                  "note", 60 + (i % 12), "amp", 0.1, "release", 0.01)
              );
            } else if (i % 3 === 1) {
              // n_set - modify node (typically direct write)
              promises.push(
                sonic.send("/n_set", nodeId - 1, "amp", 0.05)
              );
            } else {
              // n_free - free node
              promises.push(
                sonic.send("/n_free", nodeId - 2)
              );
            }
            messagesSent++;
          }

          // Wait for this burst
          await Promise.all(promises);

          // Small delay between bursts
          await new Promise(r => setTimeout(r, BURST_DELAY));
        }

        // Wait for processing to complete
        await new Promise(r => setTimeout(r, 500));

        // Check for corruption indicators in debug messages
        const corruptionErrors = debugMessages.filter(msg =>
          msg.includes("Command not found:") ||
          msg.includes("Bundle too large") ||
          msg.includes("Invalid magic") ||
          msg.includes("FAILURE IN SERVER")
        );

        return {
          success: true,
          messagesSent,
          debugCount: debugMessages.length,
          corruptionErrors,
          // Return last 20 debug messages for analysis
          recentDebug: debugMessages.slice(-20)
        };
      } catch (err) {
        return { success: false, error: err.message, debugMessages };
      }
    });

    console.log(`Sent ${result.messagesSent} messages`);
    console.log(`Debug messages: ${result.debugCount}`);

    if (result.corruptionErrors?.length > 0) {
      console.log("CORRUPTION DETECTED:");
      result.corruptionErrors.forEach(e => console.log(`  - ${e}`));
    }

    if (errors.length > 0) {
      console.log("Console errors detected:");
      errors.forEach(e => console.log(`  - ${e}`));
    }

    // The test passes if no corruption was detected
    expect(result.success).toBe(true);
    expect(result.corruptionErrors?.length || 0).toBe(0);
    expect(errors.filter(e =>
      e.includes("Command not found:") ||
      e.includes("Bundle too large") ||
      e.includes("Invalid magic")
    ).length).toBe(0);
  });

  test("concurrent timed bundles should not cause corruption", async ({ page }) => {
    // This test focuses on timed bundles which go through the scheduler
    const errors = [];

    page.on("console", (msg) => {
      const text = msg.text();
      if (text.includes("Command not found:") ||
          text.includes("Bundle too large") ||
          text.includes("Invalid magic")) {
        errors.push(text);
      }
    });

    await page.goto("/test/harness.html");

    await page.waitForFunction(() => window.supersonicReady === true, {
      timeout: 10000,
    });

    const result = await page.evaluate(async () => {
      const sonic = new window.SuperSonic({
        workerBaseURL: "/dist/workers/",
        wasmBaseURL: "/dist/wasm/",
        sampleBaseURL: "/dist/samples/",
        synthdefBaseURL: "/dist/synthdefs/",
      });

      const debugMessages = [];
      sonic.onDebug = (msg) => {
        debugMessages.push(msg);
      };

      try {
        await sonic.init();
        await sonic.loadSynthDefs(["sonic-pi-beep"]);
        await sonic.sync(1);

        // Get current NTP time
        const NTP_EPOCH_OFFSET = 2208988800;
        const getCurrentNTP = () => {
          const perfTimeMs = performance.timeOrigin + performance.now();
          return (perfTimeMs / 1000) + NTP_EPOCH_OFFSET;
        };

        // Create OSC bundle helper
        const createBundle = (ntpTime, address, ...args) => {
          const message = { address, args: args.map(a =>
            typeof a === 'string' ? { type: 's', value: a } :
            typeof a === 'number' && Number.isInteger(a) ? { type: 'i', value: a } :
            { type: 'f', value: a }
          )};

          const encodedMessage = window.SuperSonic.osc.encode(message);
          const bundleSize = 8 + 8 + 4 + encodedMessage.byteLength;
          const bundle = new Uint8Array(bundleSize);
          const view = new DataView(bundle.buffer);

          // Write "#bundle\0"
          bundle.set([0x23, 0x62, 0x75, 0x6e, 0x64, 0x6c, 0x65, 0x00], 0);

          // Write NTP timestamp
          const ntpSeconds = Math.floor(ntpTime);
          const ntpFraction = Math.floor((ntpTime % 1) * 0x100000000);
          view.setUint32(8, ntpSeconds, false);
          view.setUint32(12, ntpFraction, false);

          // Write message size and data
          view.setInt32(16, encodedMessage.byteLength, false);
          bundle.set(encodedMessage, 20);

          return bundle;
        };

        // Send many timed bundles with timestamps slightly in the future
        // Increased to stress the system more
        const NUM_BUNDLES = 2000;
        const baseNTP = getCurrentNTP();

        for (let i = 0; i < NUM_BUNDLES; i++) {
          // Schedule notes at various times in the future (10ms to 500ms)
          const futureOffset = 0.01 + (i % 50) * 0.01;
          const targetNTP = baseNTP + futureOffset;
          const nodeId = 20000 + i;

          const bundle = createBundle(
            targetNTP,
            "/s_new", "sonic-pi-beep", nodeId, 0, 0,
            "note", 60 + (i % 24), "amp", 0.05, "release", 0.01
          );

          // Send without awaiting - fire and forget
          sonic.sendOSC(bundle);
        }

        // Wait for all scheduled bundles to execute
        await new Promise(r => setTimeout(r, 1000));

        const corruptionErrors = debugMessages.filter(msg =>
          msg.includes("Command not found:") ||
          msg.includes("Bundle too large") ||
          msg.includes("Invalid magic")
        );

        return {
          success: true,
          bundlesSent: NUM_BUNDLES,
          corruptionErrors,
          recentDebug: debugMessages.slice(-20)
        };
      } catch (err) {
        return { success: false, error: err.message };
      }
    });

    console.log(`Sent ${result.bundlesSent} timed bundles`);

    if (result.corruptionErrors?.length > 0) {
      console.log("CORRUPTION DETECTED:");
      result.corruptionErrors.forEach(e => console.log(`  - ${e}`));
    }

    expect(result.success).toBe(true);
    expect(result.corruptionErrors?.length || 0).toBe(0);
    expect(errors.length).toBe(0);
  });

  test("mixed immediate and timed messages concurrently", async ({ page }) => {
    // This test simulates the actual demo behavior:
    // - Immediate n_set messages from pad movement (direct write path)
    // - Timed s_new bundles from arpeggiator (worker path)
    // Both happening at the same time = maximum race condition potential

    const errors = [];

    page.on("console", (msg) => {
      const text = msg.text();
      if (text.includes("Command not found:") ||
          text.includes("Bundle too large") ||
          text.includes("Invalid magic") ||
          text.includes("FAILURE IN SERVER")) {
        errors.push(text);
      }
    });

    await page.goto("/test/harness.html");

    await page.waitForFunction(() => window.supersonicReady === true, {
      timeout: 10000,
    });

    const result = await page.evaluate(async () => {
      const sonic = new window.SuperSonic({
        workerBaseURL: "/dist/workers/",
        wasmBaseURL: "/dist/wasm/",
        sampleBaseURL: "/dist/samples/",
        synthdefBaseURL: "/dist/synthdefs/",
      });

      const debugMessages = [];
      sonic.onDebug = (msg) => {
        debugMessages.push(msg);
      };

      try {
        await sonic.init();
        await sonic.loadSynthDefs(["sonic-pi-beep"]);
        await sonic.sync(1);

        // Create a persistent synth to send n_set messages to
        await sonic.send("/s_new", "sonic-pi-beep", 9999, 0, 0,
          "note", 60, "amp", 0.1, "sustain", 100, "release", 0.1);

        // NTP helpers
        const NTP_EPOCH_OFFSET = 2208988800;
        const getCurrentNTP = () => {
          const perfTimeMs = performance.timeOrigin + performance.now();
          return (perfTimeMs / 1000) + NTP_EPOCH_OFFSET;
        };

        const createBundle = (ntpTime, address, ...args) => {
          const message = { address, args: args.map(a =>
            typeof a === 'string' ? { type: 's', value: a } :
            typeof a === 'number' && Number.isInteger(a) ? { type: 'i', value: a } :
            { type: 'f', value: a }
          )};

          const encodedMessage = window.SuperSonic.osc.encode(message);
          const bundleSize = 8 + 8 + 4 + encodedMessage.byteLength;
          const bundle = new Uint8Array(bundleSize);
          const view = new DataView(bundle.buffer);

          bundle.set([0x23, 0x62, 0x75, 0x6e, 0x64, 0x6c, 0x65, 0x00], 0);
          const ntpSeconds = Math.floor(ntpTime);
          const ntpFraction = Math.floor((ntpTime % 1) * 0x100000000);
          view.setUint32(8, ntpSeconds, false);
          view.setUint32(12, ntpFraction, false);
          view.setInt32(16, encodedMessage.byteLength, false);
          bundle.set(encodedMessage, 20);

          return bundle;
        };

        // Run for 30 seconds of simulated interaction (original issue appeared after ~1 minute)
        const TEST_DURATION_MS = 30000;
        const startTime = performance.now();
        let immediateCount = 0;
        let timedCount = 0;

        // Simulated "arpeggiator" sending timed bundles
        const arpeggiatorInterval = setInterval(() => {
          const baseNTP = getCurrentNTP();
          // Send 4 notes at 125ms intervals (like real arpeggiator)
          for (let i = 0; i < 4; i++) {
            const bundle = createBundle(
              baseNTP + 0.05 + (i * 0.125),  // 50ms lookahead + offset
              "/s_new", "sonic-pi-beep", 30000 + timedCount, 0, 0,
              "note", 60 + (timedCount % 12), "amp", 0.05, "release", 0.01
            );
            sonic.sendOSC(bundle);
            timedCount++;
          }
        }, 100);  // Every 100ms

        // Simulated "pad movement" sending immediate n_set messages
        // This runs much faster to simulate rapid mouse movements
        const padInterval = setInterval(() => {
          // Burst of 10 n_set messages (simulating mousemove events)
          for (let i = 0; i < 10; i++) {
            sonic.send("/n_set", 9999, "amp", 0.05 + Math.random() * 0.1);
            immediateCount++;
          }
        }, 5);  // Every 5ms = 200 bursts/sec

        // Wait for test duration
        await new Promise(resolve => setTimeout(resolve, TEST_DURATION_MS));

        clearInterval(arpeggiatorInterval);
        clearInterval(padInterval);

        // Give time for queued messages to process
        await new Promise(r => setTimeout(r, 500));

        // Clean up
        await sonic.send("/n_free", 9999);

        const corruptionErrors = debugMessages.filter(msg =>
          msg.includes("Command not found:") ||
          msg.includes("Bundle too large") ||
          msg.includes("Invalid magic") ||
          msg.includes("FAILURE IN SERVER")
        );

        return {
          success: true,
          immediateCount,
          timedCount,
          corruptionErrors,
          debugCount: debugMessages.length,
          recentDebug: debugMessages.slice(-30)
        };
      } catch (err) {
        return { success: false, error: err.message };
      }
    });

    console.log(`Sent ${result.immediateCount} immediate + ${result.timedCount} timed = ${result.immediateCount + result.timedCount} total`);

    if (result.corruptionErrors?.length > 0) {
      console.log("CORRUPTION DETECTED:");
      result.corruptionErrors.forEach(e => console.log(`  - ${e}`));
    }

    if (errors.length > 0) {
      console.log("Console errors:");
      errors.forEach(e => console.log(`  - ${e}`));
    }

    expect(result.success).toBe(true);
    expect(result.corruptionErrors?.length || 0).toBe(0);
    expect(errors.filter(e =>
      e.includes("Command not found:") ||
      e.includes("Bundle too large") ||
      e.includes("Invalid magic")
    ).length).toBe(0);
  });
});
