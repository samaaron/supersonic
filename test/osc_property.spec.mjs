/**
 * Property-based tests for osc_fast.js using fast-check.
 *
 * These complement the example-based tests in osc_fast.spec.mjs by
 * generating random inputs to find edge cases in encoding/decoding.
 */

import { test, expect } from '@playwright/test';

const NUM_RUNS = 500;

test.describe('OSC Property-Based Tests', () => {
  test.beforeEach(async ({ page }) => {
    await page.goto('/test/osc_harness.html');
    await page.waitForFunction(() => window.oscReady === true, {
      timeout: 5000,
    });
  });

  // ===========================================================================
  // MESSAGE ROUNDTRIP PROPERTIES
  // ===========================================================================

  test.describe('Message Roundtrip', () => {
    test('integer args roundtrip exactly', async ({ page }) => {
      await page.evaluate((numRuns) => {
        const fc = window.fc;
        const osc = window.oscFast;

        fc.assert(fc.property(
          fc.integer({ min: -2147483648, max: 2147483647 }),
          (n) => {
            const encoded = osc.copyEncoded(osc.encodeMessage('/t', [n]));
            const decoded = osc.decodeMessage(encoded);
            if (decoded[0] !== '/t') return false;
            if (decoded[1] !== n) return false;
            return true;
          }
        ), { numRuns });
      }, NUM_RUNS);
    });

    test('float args roundtrip to float32 precision', async ({ page }) => {
      await page.evaluate((numRuns) => {
        const fc = window.fc;
        const osc = window.oscFast;

        fc.assert(fc.property(
          fc.double({ noNaN: true, noDefaultInfinity: true, min: -1e38, max: 1e38 }),
          (f) => {
            const encoded = osc.copyEncoded(osc.encodeMessage('/t', [{ type: 'float', value: f }]));
            const decoded = osc.decodeMessage(encoded);
            if (decoded[0] !== '/t') return false;
            // float32 precision
            const expected = Math.fround(f);
            if (decoded[1] !== expected) return false;
            return true;
          }
        ), { numRuns });
      }, NUM_RUNS);
    });

    test('double args roundtrip exactly', async ({ page }) => {
      await page.evaluate((numRuns) => {
        const fc = window.fc;
        const osc = window.oscFast;

        fc.assert(fc.property(
          fc.double({ noNaN: true }),
          (d) => {
            const encoded = osc.copyEncoded(osc.encodeMessage('/t', [{ type: 'double', value: d }]));
            const decoded = osc.decodeMessage(encoded);
            if (decoded[0] !== '/t') return false;
            if (decoded[1] !== d) return false;
            return true;
          }
        ), { numRuns });
      }, NUM_RUNS);
    });

    test('ASCII string args roundtrip exactly', async ({ page }) => {
      await page.evaluate((numRuns) => {
        const fc = window.fc;
        const osc = window.oscFast;

        // ASCII printable characters (0x20-0x7E), no null bytes
        const asciiString = fc.stringMatching(/^[\x20-\x7e]{0,200}$/);

        fc.assert(fc.property(asciiString, (s) => {
          const encoded = osc.copyEncoded(osc.encodeMessage('/t', [s]));
          const decoded = osc.decodeMessage(encoded);
          if (decoded[0] !== '/t') return false;
          if (decoded[1] !== s) return false;
          return true;
        }), { numRuns });
      }, NUM_RUNS);
    });

    test('UTF-8 string args roundtrip exactly', async ({ page }) => {
      await page.evaluate((numRuns) => {
        const fc = window.fc;
        const osc = window.oscFast;

        // Unicode strings (excluding null and surrogate pairs)
        const unicodeString = fc.string({ minLength: 0, maxLength: 100 })
          .filter(s => !s.includes('\0'));

        fc.assert(fc.property(unicodeString, (s) => {
          const encoded = osc.copyEncoded(osc.encodeMessage('/t', [s]));
          const decoded = osc.decodeMessage(encoded);
          if (decoded[0] !== '/t') return false;
          if (decoded[1] !== s) return false;
          return true;
        }), { numRuns });
      }, NUM_RUNS);
    });

    test('boolean args roundtrip exactly', async ({ page }) => {
      await page.evaluate((numRuns) => {
        const fc = window.fc;
        const osc = window.oscFast;

        fc.assert(fc.property(fc.boolean(), (b) => {
          const encoded = osc.copyEncoded(osc.encodeMessage('/t', [b]));
          const decoded = osc.decodeMessage(encoded);
          if (decoded[0] !== '/t') return false;
          if (decoded[1] !== b) return false;
          return true;
        }), { numRuns });
      }, NUM_RUNS);
    });

    test('blob args roundtrip exactly', async ({ page }) => {
      await page.evaluate((numRuns) => {
        const fc = window.fc;
        const osc = window.oscFast;

        const blob = fc.uint8Array({ minLength: 0, maxLength: 500 });

        fc.assert(fc.property(blob, (bytes) => {
          const encoded = osc.copyEncoded(osc.encodeMessage('/t', [bytes]));
          const decoded = osc.decodeMessage(encoded);
          if (decoded[0] !== '/t') return false;
          const result = decoded[1];
          if (result.length !== bytes.length) return false;
          for (let i = 0; i < bytes.length; i++) {
            if (result[i] !== bytes[i]) return false;
          }
          return true;
        }), { numRuns });
      }, NUM_RUNS);
    });

    test('mixed arg types roundtrip correctly', async ({ page }) => {
      await page.evaluate((numRuns) => {
        const fc = window.fc;
        const osc = window.oscFast;

        // Generate a random OSC arg (int, string, or bool)
        // Using types that roundtrip exactly (no float precision issues)
        const oscArg = fc.oneof(
          fc.integer({ min: -2147483648, max: 2147483647 }),
          fc.stringMatching(/^[\x20-\x7e]{0,50}$/),
          fc.boolean(),
        );

        const oscArgs = fc.array(oscArg, { minLength: 0, maxLength: 10 });

        fc.assert(fc.property(oscArgs, (args) => {
          const encoded = osc.copyEncoded(osc.encodeMessage('/test', args));
          const decoded = osc.decodeMessage(encoded);
          if (decoded[0] !== '/test') return false;
          if (decoded.length !== args.length + 1) return false;
          for (let i = 0; i < args.length; i++) {
            if (decoded[i + 1] !== args[i]) return false;
          }
          return true;
        }), { numRuns });
      }, NUM_RUNS);
    });
  });

  // ===========================================================================
  // STRUCTURAL INVARIANTS
  // ===========================================================================

  test.describe('Structural Invariants', () => {
    test('encoded messages are always 4-byte aligned', async ({ page }) => {
      await page.evaluate((numRuns) => {
        const fc = window.fc;
        const osc = window.oscFast;

        // Generate address strings of varying lengths
        const oscAddress = fc.integer({ min: 1, max: 100 }).map(len => {
          return '/' + 'a'.repeat(len);
        });

        const oscArg = fc.oneof(
          fc.integer({ min: -2147483648, max: 2147483647 }),
          fc.stringMatching(/^[\x20-\x7e]{0,50}$/),
          fc.boolean(),
          fc.uint8Array({ minLength: 0, maxLength: 100 }),
        );

        const oscArgs = fc.array(oscArg, { minLength: 0, maxLength: 8 });

        fc.assert(fc.property(oscAddress, oscArgs, (addr, args) => {
          const encoded = osc.encodeMessage(addr, args);
          return encoded.length % 4 === 0;
        }), { numRuns });
      }, NUM_RUNS);
    });

    test('encoded bundles are always 4-byte aligned', async ({ page }) => {
      await page.evaluate((numRuns) => {
        const fc = window.fc;
        const osc = window.oscFast;

        const simpleMessage = fc.tuple(
          fc.integer({ min: 1, max: 20 }).map(n => '/' + 'x'.repeat(n)),
          fc.array(fc.integer({ min: -1000, max: 1000 }), { minLength: 0, maxLength: 5 })
        ).map(([addr, args]) => [addr, ...args]);

        const messages = fc.array(simpleMessage, { minLength: 1, maxLength: 5 });

        fc.assert(fc.property(messages, (msgs) => {
          const encoded = osc.encodeBundle(1, msgs);
          return encoded.length % 4 === 0;
        }), { numRuns });
      }, NUM_RUNS);
    });

    test('decodePacket detects messages vs bundles correctly', async ({ page }) => {
      await page.evaluate((numRuns) => {
        const fc = window.fc;
        const osc = window.oscFast;

        // Messages should decode as arrays
        fc.assert(fc.property(
          fc.integer({ min: -2147483648, max: 2147483647 }),
          (n) => {
            const encoded = osc.copyEncoded(osc.encodeMessage('/t', [n]));
            const decoded = osc.decodePacket(encoded);
            return Array.isArray(decoded);
          }
        ), { numRuns });

        // Bundles should decode as objects with timeTag and packets
        fc.assert(fc.property(
          fc.integer({ min: -2147483648, max: 2147483647 }),
          (n) => {
            const encoded = osc.copyEncoded(osc.encodeBundle(1, [['/t', n]]));
            const decoded = osc.decodePacket(encoded);
            return !Array.isArray(decoded) &&
              typeof decoded.timeTag === 'number' &&
              Array.isArray(decoded.packets);
          }
        ), { numRuns });
      }, NUM_RUNS);
    });
  });

  // ===========================================================================
  // ADDRESS STRING PADDING
  // ===========================================================================

  test.describe('Address Padding', () => {
    test('addresses of all lengths 1-64 roundtrip correctly', async ({ page }) => {
      await page.evaluate(() => {
        const osc = window.oscFast;

        // Exhaustively test address lengths 1-64
        // The 4-byte alignment padding is the tricky part
        for (let len = 1; len <= 64; len++) {
          const addr = '/' + 'a'.repeat(len - 1);
          const encoded = osc.copyEncoded(osc.encodeMessage(addr, [42]));

          if (encoded.length % 4 !== 0) {
            throw new Error(`Address length ${len}: encoded not 4-byte aligned (${encoded.length} bytes)`);
          }

          const decoded = osc.decodeMessage(encoded);
          if (decoded[0] !== addr) {
            throw new Error(`Address length ${len}: roundtrip failed. Got "${decoded[0]}" expected "${addr}"`);
          }
          if (decoded[1] !== 42) {
            throw new Error(`Address length ${len}: arg roundtrip failed. Got ${decoded[1]} expected 42`);
          }
        }
      });
    });

    test('string args of all lengths 0-64 roundtrip correctly', async ({ page }) => {
      await page.evaluate(() => {
        const osc = window.oscFast;

        for (let len = 0; len <= 64; len++) {
          const s = 'b'.repeat(len);
          const encoded = osc.copyEncoded(osc.encodeMessage('/t', [s]));

          if (encoded.length % 4 !== 0) {
            throw new Error(`String length ${len}: encoded not 4-byte aligned (${encoded.length} bytes)`);
          }

          const decoded = osc.decodeMessage(encoded);
          if (decoded[1] !== s) {
            throw new Error(`String length ${len}: roundtrip failed`);
          }
        }
      });
    });

    test('blob sizes 0-64 roundtrip correctly', async ({ page }) => {
      await page.evaluate(() => {
        const osc = window.oscFast;

        for (let len = 0; len <= 64; len++) {
          const blob = new Uint8Array(len);
          for (let i = 0; i < len; i++) blob[i] = i & 0xFF;

          const encoded = osc.copyEncoded(osc.encodeMessage('/t', [blob]));

          if (encoded.length % 4 !== 0) {
            throw new Error(`Blob size ${len}: encoded not 4-byte aligned (${encoded.length} bytes)`);
          }

          const decoded = osc.decodeMessage(encoded);
          const result = decoded[1];
          if (result.length !== len) {
            throw new Error(`Blob size ${len}: length mismatch (got ${result.length})`);
          }
          for (let i = 0; i < len; i++) {
            if (result[i] !== (i & 0xFF)) {
              throw new Error(`Blob size ${len}: byte ${i} mismatch`);
            }
          }
        }
      });
    });
  });

  // ===========================================================================
  // BUNDLE ROUNDTRIP PROPERTIES
  // ===========================================================================

  test.describe('Bundle Roundtrip', () => {
    test('single-message bundles roundtrip via encodeSingleBundle', async ({ page }) => {
      await page.evaluate((numRuns) => {
        const fc = window.fc;
        const osc = window.oscFast;

        fc.assert(fc.property(
          fc.array(fc.integer({ min: -2147483648, max: 2147483647 }), { minLength: 0, maxLength: 8 }),
          (args) => {
            const encoded = osc.copyEncoded(osc.encodeSingleBundle(1, '/test', args));
            const decoded = osc.decodeBundle(encoded);
            if (decoded.packets.length !== 1) return false;
            const msg = decoded.packets[0];
            if (msg[0] !== '/test') return false;
            if (msg.length !== args.length + 1) return false;
            for (let i = 0; i < args.length; i++) {
              if (msg[i + 1] !== args[i]) return false;
            }
            return true;
          }
        ), { numRuns });
      }, NUM_RUNS);
    });

    test('multi-message bundles roundtrip correctly', async ({ page }) => {
      await page.evaluate((numRuns) => {
        const fc = window.fc;
        const osc = window.oscFast;

        const oscMessage = fc.tuple(
          fc.integer({ min: 1, max: 10 }).map(n => '/' + String.fromCharCode(97 + (n % 26))),
          fc.array(fc.integer({ min: -1000, max: 1000 }), { minLength: 0, maxLength: 4 })
        ).map(([addr, args]) => [addr, ...args]);

        const messages = fc.array(oscMessage, { minLength: 1, maxLength: 8 });

        fc.assert(fc.property(messages, (msgs) => {
          const encoded = osc.copyEncoded(osc.encodeBundle(1, msgs));
          const decoded = osc.decodeBundle(encoded);
          if (decoded.packets.length !== msgs.length) return false;
          for (let i = 0; i < msgs.length; i++) {
            const original = msgs[i];
            const result = decoded.packets[i];
            if (result.length !== original.length) return false;
            if (result[0] !== original[0]) return false;
            for (let j = 1; j < original.length; j++) {
              if (result[j] !== original[j]) return false;
            }
          }
          return true;
        }), { numRuns });
      }, NUM_RUNS);
    });

    test('timetag [sec, frac] array roundtrips with full precision', async ({ page }) => {
      await page.evaluate((numRuns) => {
        const fc = window.fc;
        const osc = window.oscFast;

        fc.assert(fc.property(
          fc.integer({ min: 0, max: 0xFFFFFFFF }),
          fc.integer({ min: 0, max: 0xFFFFFFFF }),
          (sec, frac) => {
            const encoded = osc.copyEncoded(osc.encodeBundle([sec, frac], [['/t']]));
            // Read raw timetag bytes
            const view = new DataView(encoded.buffer, encoded.byteOffset, encoded.byteLength);
            const readSec = view.getUint32(8, false);
            const readFrac = view.getUint32(12, false);
            return readSec === (sec >>> 0) && readFrac === (frac >>> 0);
          }
        ), { numRuns });
      }, NUM_RUNS);
    });
  });

  // ===========================================================================
  // ENCODE/DECODE CONSISTENCY
  // ===========================================================================

  test.describe('Encode/Decode Consistency', () => {
    test('encodeBundle and encodeSingleBundle produce identical output', async ({ page }) => {
      await page.evaluate((numRuns) => {
        const fc = window.fc;
        const osc = window.oscFast;

        fc.assert(fc.property(
          fc.array(fc.integer({ min: -2147483648, max: 2147483647 }), { minLength: 0, maxLength: 5 }),
          (args) => {
            const bundled = osc.copyEncoded(osc.encodeBundle(1, [['/t', ...args]]));
            const single = osc.copyEncoded(osc.encodeSingleBundle(1, '/t', args));
            if (bundled.length !== single.length) return false;
            for (let i = 0; i < bundled.length; i++) {
              if (bundled[i] !== single[i]) return false;
            }
            return true;
          }
        ), { numRuns });
      }, NUM_RUNS);
    });

    test('getBundleTimeTag matches decoded bundle timeTag', async ({ page }) => {
      await page.evaluate((numRuns) => {
        const fc = window.fc;
        const osc = window.oscFast;

        fc.assert(fc.property(
          fc.integer({ min: 1, max: 0xFFFFFFFF }),
          (sec) => {
            const encoded = osc.copyEncoded(osc.encodeBundle([sec, 0], [['/t']]));
            const quick = osc.getBundleTimeTag(encoded);
            const full = osc.decodeBundle(encoded).timeTag;
            return quick === full;
          }
        ), { numRuns });
      }, NUM_RUNS);
    });

    test('isBundle is true for bundles, false for messages', async ({ page }) => {
      await page.evaluate((numRuns) => {
        const fc = window.fc;
        const osc = window.oscFast;

        // Bundles
        fc.assert(fc.property(
          fc.integer({ min: 0, max: 100 }),
          () => {
            const encoded = osc.copyEncoded(osc.encodeBundle(1, [['/t']]));
            return osc.isBundle(encoded) === true;
          }
        ), { numRuns: 10 });

        // Messages
        fc.assert(fc.property(
          fc.integer({ min: 1, max: 50 }).map(n => '/' + 'z'.repeat(n)),
          (addr) => {
            const encoded = osc.copyEncoded(osc.encodeMessage(addr, []));
            return osc.isBundle(encoded) === false;
          }
        ), { numRuns });
      }, NUM_RUNS);
    });
  });
});
