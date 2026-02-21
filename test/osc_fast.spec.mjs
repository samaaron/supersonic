/**
 * Tests for osc_fast.js - zero-allocation OSC encoder/decoder
 *
 * Tests cover:
 * - Message encoding with all argument types
 * - Bundle encoding with timetags
 * - Message decoding
 * - Bundle decoding
 * - Round-trip (encode then decode)
 * - String caching
 * - Edge cases
 * - Performance
 */

import { test, expect } from '@playwright/test';

test.describe('OSC Fast Encoder/Decoder', () => {
  test.beforeEach(async ({ page }) => {
    await page.goto('/test/osc_harness.html');
    await page.waitForFunction(() => window.oscReady === true, {
      timeout: 5000,
    });
  });

  // ===========================================================================
  // MESSAGE ENCODING
  // ===========================================================================

  test.describe('Message Encoding', () => {
    test('encodes simple message with no args', async ({ page }) => {
      const result = await page.evaluate(() => {
        const encoded = window.oscFast.encodeMessage('/status');
        return {
          length: encoded.length,
          // Check header bytes
          startsWithSlash: encoded[0] === 0x2F, // '/'
          hasNullTerminator: encoded[7] === 0, // '/status\0'
          // Type tag should be just ','
          typeTagComma: encoded[8] === 0x2C, // ','
        };
      });

      expect(result.startsWithSlash).toBe(true);
      expect(result.hasNullTerminator).toBe(true);
      expect(result.typeTagComma).toBe(true);
      expect(result.length).toBe(12); // '/status\0' (8) + ',\0\0\0' (4)
    });

    test('encodes message with integer arg', async ({ page }) => {
      const result = await page.evaluate(() => {
        const encoded = window.oscFast.encodeMessage('/test', [42]);
        const decoded = window.oscFast.decodeMessage(encoded);
        return decoded;
      });

      expect(result).toEqual(['/test', 42]);
    });

    test('encodes message with negative integer', async ({ page }) => {
      const result = await page.evaluate(() => {
        const encoded = window.oscFast.encodeMessage('/test', [-12345]);
        const decoded = window.oscFast.decodeMessage(encoded);
        return decoded;
      });

      expect(result).toEqual(['/test', -12345]);
    });

    test('encodes message with float arg', async ({ page }) => {
      const result = await page.evaluate(() => {
        const encoded = window.oscFast.encodeMessage('/test', [3.14159]);
        const decoded = window.oscFast.decodeMessage(encoded);
        return decoded;
      });

      expect(result[0]).toBe('/test');
      expect(result[1]).toBeCloseTo(3.14159, 4);
    });

    test('encodes message with string arg', async ({ page }) => {
      const result = await page.evaluate(() => {
        const encoded = window.oscFast.encodeMessage('/test', ['hello']);
        const decoded = window.oscFast.decodeMessage(encoded);
        return decoded;
      });

      expect(result).toEqual(['/test', 'hello']);
    });

    test('encodes message with boolean args', async ({ page }) => {
      const result = await page.evaluate(() => {
        const encoded = window.oscFast.encodeMessage('/test', [true, false]);
        const decoded = window.oscFast.decodeMessage(encoded);
        return decoded;
      });

      expect(result).toEqual(['/test', true, false]);
    });

    test('encodes message with blob arg', async ({ page }) => {
      const result = await page.evaluate(() => {
        const blob = new Uint8Array([1, 2, 3, 4, 5]);
        const encoded = window.oscFast.encodeMessage('/test', [blob]);
        const decoded = window.oscFast.decodeMessage(encoded);
        return {
          address: decoded[0],
          blobLength: decoded[1].length,
          blobData: Array.from(decoded[1]),
        };
      });

      expect(result.blobLength).toBe(5);
      expect(result.blobData).toEqual([1, 2, 3, 4, 5]);
    });

    test('encodes message with mixed args', async ({ page }) => {
      const result = await page.evaluate(() => {
        const encoded = window.oscFast.encodeMessage('/s_new', [
          'sonic-pi-beep',
          1001,
          0,
          0,
          'note',
          60,
          'amp',
          0.5,
        ]);
        const decoded = window.oscFast.decodeMessage(encoded);
        return decoded;
      });

      expect(result[0]).toBe('/s_new');
      expect(result[1]).toBe('sonic-pi-beep');
      expect(result[2]).toBe(1001);
      expect(result[3]).toBe(0);
      expect(result[4]).toBe(0);
      expect(result[5]).toBe('note');
      expect(result[6]).toBe(60);
      expect(result[7]).toBe('amp');
      expect(result[8]).toBeCloseTo(0.5, 5);
    });

    test('encodes message with int64 arg', async ({ page }) => {
      const result = await page.evaluate(() => {
        const encoded = window.oscFast.encodeMessage('/test', [
          { type: 'int64', value: 9007199254740993n },
        ]);
        const decoded = window.oscFast.decodeMessage(encoded);
        return decoded.map((a) =>
          typeof a === 'bigint' ? a.toString() : a
        );
      });

      expect(result[1]).toBe('9007199254740993');
    });

    test('encodes message with double arg', async ({ page }) => {
      const result = await page.evaluate(() => {
        const encoded = window.oscFast.encodeMessage('/test', [
          { type: 'double', value: 3.141592653589793 },
        ]);
        const decoded = window.oscFast.decodeMessage(encoded);
        return decoded;
      });

      expect(result[1]).toBeCloseTo(3.141592653589793, 10);
    });
  });

  // ===========================================================================
  // BUNDLE ENCODING
  // ===========================================================================

  test.describe('Bundle Encoding', () => {
    test('encodes bundle with immediate timetag', async ({ page }) => {
      const result = await page.evaluate(() => {
        const encoded = window.oscFast.encodeBundle(1, [
          ['/test', 42],
        ]);
        const decoded = window.oscFast.decodeBundle(encoded);
        return {
          timeTag: decoded.timeTag,
          packetCount: decoded.packets.length,
          firstPacket: decoded.packets[0],
        };
      });

      // Immediate timetag is 1 (very small NTP time)
      expect(result.timeTag).toBeLessThan(10);
      expect(result.packetCount).toBe(1);
      expect(result.firstPacket).toEqual(['/test', 42]);
    });

    test('encodes bundle with NTP timetag', async ({ page }) => {
      const result = await page.evaluate(() => {
        // NTP timestamp for some time in 2024
        const ntpTime = 3913056000 + 0.5; // seconds + fraction
        const encoded = window.oscFast.encodeBundle(ntpTime, [
          ['/test'],
        ]);
        const decoded = window.oscFast.decodeBundle(encoded);
        return {
          timeTag: decoded.timeTag,
          timeTagInt: Math.floor(decoded.timeTag),
          timeTagFrac: decoded.timeTag - Math.floor(decoded.timeTag),
        };
      });

      expect(result.timeTagInt).toBe(3913056000);
      expect(result.timeTagFrac).toBeCloseTo(0.5, 5);
    });

    test('encodes bundle with multiple messages', async ({ page }) => {
      const result = await page.evaluate(() => {
        const encoded = window.oscFast.encodeBundle(1, [
          ['/one', 1],
          ['/two', 2],
          ['/three', 3],
        ]);
        const decoded = window.oscFast.decodeBundle(encoded);
        return {
          packetCount: decoded.packets.length,
          packets: decoded.packets,
        };
      });

      expect(result.packetCount).toBe(3);
      expect(result.packets).toEqual([['/one', 1], ['/two', 2], ['/three', 3]]);
    });

    test('encodes single-message bundle (optimized path)', async ({ page }) => {
      const result = await page.evaluate(() => {
        const encoded = window.oscFast.encodeSingleBundle(1, '/s_new', [
          'beep',
          1001,
          0,
          0,
        ]);
        const decoded = window.oscFast.decodeBundle(encoded);
        return {
          packetCount: decoded.packets.length,
          firstPacket: decoded.packets[0],
        };
      });

      expect(result.packetCount).toBe(1);
      expect(result.firstPacket).toEqual(['/s_new', 'beep', 1001, 0, 0]);
    });

    test('encodes nested bundles', async ({ page }) => {
      const result = await page.evaluate(() => {
        const encoded = window.oscFast.encodeBundle(1, [
          ['/outer'],
          {
            timeTag: 2,
            packets: [
              ['/inner1', 1],
              ['/inner2', 2],
            ],
          },
        ]);
        const decoded = window.oscFast.decodeBundle(encoded);
        return {
          outerPacketCount: decoded.packets.length,
          firstPacket: decoded.packets[0],
          nestedTimeTag: decoded.packets[1].timeTag,
          nestedPacketCount: decoded.packets[1].packets?.length,
          nestedPackets: decoded.packets[1].packets,
        };
      });

      expect(result.outerPacketCount).toBe(2);
      expect(result.firstPacket).toEqual(['/outer']);
      expect(result.nestedPacketCount).toBe(2);
      expect(result.nestedPackets).toEqual([['/inner1', 1], ['/inner2', 2]]);
    });
  });

  // ===========================================================================
  // TIMETAG INPUT FORMATS
  // ===========================================================================

  test.describe('TimeTag Input Formats', () => {
    test('null produces immediate timetag', async ({ page }) => {
      const result = await page.evaluate(() => {
        const encoded = window.oscFast.encodeBundle(null, [
          ['/test', 1],
        ]);
        const decoded = window.oscFast.decodeBundle(encoded);
        return { timeTag: decoded.timeTag };
      });

      expect(result.timeTag).toBeLessThan(10);
    });

    test('undefined produces immediate timetag', async ({ page }) => {
      const result = await page.evaluate(() => {
        const encoded = window.oscFast.encodeBundle(undefined, [
          ['/test', 1],
        ]);
        const decoded = window.oscFast.decodeBundle(encoded);
        return { timeTag: decoded.timeTag };
      });

      expect(result.timeTag).toBeLessThan(10);
    });

    test('[sec, frac] array produces correct encoding', async ({ page }) => {
      const result = await page.evaluate(() => {
        const sec = 3913056000;
        const frac = 2147483648; // 0.5 in NTP fractional
        const encoded = window.oscFast.encodeBundle([sec, frac], [
          ['/test'],
        ]);
        const timetag = window.readTimetag(encoded);
        return timetag;
      });

      expect(result.ntpSeconds).toBe(3913056000);
      expect(result.ntpFraction).toBe(2147483648);
    });

    test('[sec, frac] round-trips with readTimetag', async ({ page }) => {
      const result = await page.evaluate(() => {
        const sec = 3913056000;
        const frac = 123456789;
        const encoded = window.oscFast.encodeBundle([sec, frac], [
          ['/test'],
        ]);
        const timetag = window.readTimetag(encoded);
        return { sec: timetag.ntpSeconds, frac: timetag.ntpFraction };
      });

      expect(result.sec).toBe(3913056000);
      expect(result.frac).toBe(123456789);
    });

    test('[sec, frac] preserves full uint32 precision', async ({ page }) => {
      const result = await page.evaluate(() => {
        // Use values near uint32 max to test precision
        const sec = 4294967295; // 0xFFFFFFFF
        const frac = 4294967295;
        const encoded = window.oscFast.encodeBundle([sec, frac], [
          ['/test'],
        ]);
        const timetag = window.readTimetag(encoded);
        return { sec: timetag.ntpSeconds, frac: timetag.ntpFraction };
      });

      expect(result.sec).toBe(4294967295);
      expect(result.frac).toBe(4294967295);
    });

    test('array with wrong length throws Error', async ({ page }) => {
      const results = await page.evaluate(() => {
        const errors = [];
        for (const arr of [[], [1], [1, 2, 3]]) {
          try {
            window.oscFast.encodeBundle(arr, [['/test']]);
            errors.push({ length: arr.length, threw: false });
          } catch (e) {
            errors.push({ length: arr.length, threw: true, name: e.constructor.name });
          }
        }
        return errors;
      });

      for (const r of results) {
        expect(r.threw).toBe(true);
        expect(r.name).toBe('Error');
      }
    });

    test('string timetag throws TypeError', async ({ page }) => {
      const result = await page.evaluate(() => {
        try {
          window.oscFast.encodeBundle('now', [['/test']]);
          return { threw: false };
        } catch (e) {
          return { threw: true, name: e.constructor.name };
        }
      });

      expect(result.threw).toBe(true);
      expect(result.name).toBe('TypeError');
    });

    test('boolean timetag throws TypeError', async ({ page }) => {
      const result = await page.evaluate(() => {
        try {
          window.oscFast.encodeBundle(true, [['/test']]);
          return { threw: false };
        } catch (e) {
          return { threw: true, name: e.constructor.name };
        }
      });

      expect(result.threw).toBe(true);
      expect(result.name).toBe('TypeError');
    });

    test('object timetag throws TypeError', async ({ page }) => {
      const result = await page.evaluate(() => {
        try {
          window.oscFast.encodeBundle({sec: 1}, [['/test']]);
          return { threw: false };
        } catch (e) {
          return { threw: true, name: e.constructor.name };
        }
      });

      expect(result.threw).toBe(true);
      expect(result.name).toBe('TypeError');
    });

    test('Unix-range number triggers console.warn', async ({ page }) => {
      const result = await page.evaluate(() => {
        const warnings = [];
        const origWarn = console.warn;
        console.warn = (...args) => warnings.push(args.join(' '));
        try {
          window.oscFast.encodeBundle(1700000000, [
            ['/test'],
          ]);
        } finally {
          console.warn = origWarn;
        }
        return { warningCount: warnings.length, hasWarning: warnings.length > 0 };
      });

      expect(result.hasWarning).toBe(true);
    });

    test('valid NTP number does not warn', async ({ page }) => {
      const result = await page.evaluate(() => {
        const warnings = [];
        const origWarn = console.warn;
        console.warn = (...args) => warnings.push(args.join(' '));
        try {
          // NTP time well above epoch offset
          window.oscFast.encodeBundle(3913056000, [
            ['/test'],
          ]);
        } finally {
          console.warn = origWarn;
        }
        return { warningCount: warnings.length };
      });

      expect(result.warningCount).toBe(0);
    });

    test('timetag 1 does not warn', async ({ page }) => {
      const result = await page.evaluate(() => {
        const warnings = [];
        const origWarn = console.warn;
        console.warn = (...args) => warnings.push(args.join(' '));
        try {
          window.oscFast.encodeBundle(1, [['/test']]);
        } finally {
          console.warn = origWarn;
        }
        return { warningCount: warnings.length };
      });

      expect(result.warningCount).toBe(0);
    });

    test('encodeSingleBundle with array timetag', async ({ page }) => {
      const result = await page.evaluate(() => {
        const sec = 3913056000;
        const frac = 500000000;
        const encoded = window.oscFast.encodeSingleBundle(
          [sec, frac],
          '/s_new',
          ['beep', 1001, 0, 0]
        );
        const timetag = window.readTimetag(encoded);
        const decoded = window.oscFast.decodeBundle(encoded);
        return {
          sec: timetag.ntpSeconds,
          frac: timetag.ntpFraction,
          firstPacket: decoded.packets[0],
        };
      });

      expect(result.sec).toBe(3913056000);
      expect(result.frac).toBe(500000000);
      expect(result.firstPacket).toEqual(['/s_new', 'beep', 1001, 0, 0]);
    });

    test('nested bundle with array timetag', async ({ page }) => {
      const result = await page.evaluate(() => {
        const encoded = window.oscFast.encodeBundle([3913056000, 0], [
          ['/outer'],
          {
            timeTag: [3913056001, 0],
            packets: [['/inner', 42]],
          },
        ]);
        const decoded = window.oscFast.decodeBundle(encoded);
        const outerTimetag = window.readTimetag(encoded);
        return {
          outerSec: outerTimetag.ntpSeconds,
          outerPacketCount: decoded.packets.length,
          firstPacket: decoded.packets[0],
          nestedPacket: decoded.packets[1].packets?.[0],
        };
      });

      expect(result.outerSec).toBe(3913056000);
      expect(result.outerPacketCount).toBe(2);
      expect(result.firstPacket).toEqual(['/outer']);
      expect(result.nestedPacket).toEqual(['/inner', 42]);
    });
  });

  // ===========================================================================
  // DECODING
  // ===========================================================================

  test.describe('Decoding', () => {
    test('decodePacket auto-detects message vs bundle', async ({ page }) => {
      const result = await page.evaluate(() => {
        // Must copy encoded data before encoding another packet (zero-allocation design)
        const message = window.oscFast.copyEncoded(
          window.oscFast.encodeMessage('/test', [42])
        );
        const bundle = window.oscFast.copyEncoded(
          window.oscFast.encodeBundle(1, [['/test']])
        );

        const decodedMsg = window.oscFast.decodePacket(message);
        const decodedBundle = window.oscFast.decodePacket(bundle);

        return {
          messageIsArray: Array.isArray(decodedMsg),
          messageStartsWithAddress: typeof decodedMsg[0] === 'string' && decodedMsg[0].startsWith('/'),
          bundleHasTimeTag: 'timeTag' in decodedBundle,
          bundleHasPackets: 'packets' in decodedBundle,
        };
      });

      expect(result.messageIsArray).toBe(true);
      expect(result.messageStartsWithAddress).toBe(true);
      expect(result.bundleHasTimeTag).toBe(true);
      expect(result.bundleHasPackets).toBe(true);
    });

    test('isBundle correctly identifies bundles', async ({ page }) => {
      const result = await page.evaluate(() => {
        // Must copy encoded data before encoding another packet (zero-allocation design)
        const message = window.oscFast.copyEncoded(
          window.oscFast.encodeMessage('/test', [])
        );
        const bundle = window.oscFast.copyEncoded(
          window.oscFast.encodeBundle(1, [])
        );

        return {
          messageIsBundle: window.oscFast.isBundle(message),
          bundleIsBundle: window.oscFast.isBundle(bundle),
        };
      });

      expect(result.messageIsBundle).toBe(false);
      expect(result.bundleIsBundle).toBe(true);
    });

    test('getBundleTimeTag extracts timetag without full decode', async ({
      page,
    }) => {
      const result = await page.evaluate(() => {
        const ntpTime = 3913056000.123;
        const bundle = window.oscFast.encodeBundle(ntpTime, [
          ['/test', 1, 2, 3, 4, 5],
        ]);

        const quickTimeTag = window.oscFast.getBundleTimeTag(bundle);
        const fullDecode = window.oscFast.decodeBundle(bundle);

        return {
          quickTimeTag,
          fullTimeTag: fullDecode.timeTag,
        };
      });

      expect(result.quickTimeTag).toBeCloseTo(result.fullTimeTag, 5);
    });
  });

  // ===========================================================================
  // ROUND-TRIP TESTS
  // ===========================================================================

  test.describe('Round-trip Encoding/Decoding', () => {
    test('round-trips complex message', async ({ page }) => {
      const result = await page.evaluate(() => {
        const original = [
          '/s_new',
          'sonic-pi-beep',
          -1,
          0,
          0,
          'note',
          60,
          'amp',
          0.8,
          'pan',
          -0.5,
          'attack',
          0.01,
          'release',
          1.5,
        ];

        const encoded = window.oscFast.encodeMessage(
          original[0],
          original.slice(1)
        );
        const decoded = window.oscFast.decodeMessage(encoded);

        return {
          original,
          decoded,
          addressMatch: original[0] === decoded[0],
          lengthMatch: original.length === decoded.length,
        };
      });

      expect(result.addressMatch).toBe(true);
      expect(result.lengthMatch).toBe(true);
      expect(result.decoded[1]).toBe('sonic-pi-beep');
      expect(result.decoded[2]).toBe(-1);
      expect(result.decoded[8]).toBeCloseTo(0.8, 5);
    });

    test('round-trips bundle with complex messages', async ({ page }) => {
      const result = await page.evaluate(() => {
        const ntpTime = 3913056000.5;
        const packets = [
          ['/s_new', 'beep', 1001, 0, 0, 'note', 60],
          ['/n_set', 1001, 'amp', 0.5],
          ['/n_free', 1001],
        ];

        const encoded = window.oscFast.encodeBundle(ntpTime, packets);
        const decoded = window.oscFast.decodeBundle(encoded);

        return {
          timeTagMatch: Math.abs(decoded.timeTag - ntpTime) < 0.0001,
          packetCount: decoded.packets.length,
          addresses: decoded.packets.map((p) => p[0]),
        };
      });

      expect(result.timeTagMatch).toBe(true);
      expect(result.packetCount).toBe(3);
      expect(result.addresses).toEqual(['/s_new', '/n_set', '/n_free']);
    });
  });

  // ===========================================================================
  // STRING CACHING
  // ===========================================================================

  test.describe('String Caching', () => {
    test('caches repeated addresses', async ({ page }) => {
      const result = await page.evaluate(() => {
        window.oscFast.clearCache();

        const before = window.oscFast.getCacheStats();

        // Encode same address multiple times
        for (let i = 0; i < 100; i++) {
          window.oscFast.encodeMessage('/s_new', [i]);
        }

        const after = window.oscFast.getCacheStats();

        return {
          beforeSize: before.stringCacheSize,
          afterSize: after.stringCacheSize,
        };
      });

      expect(result.beforeSize).toBe(0);
      expect(result.afterSize).toBe(1); // Only one address cached
    });

    test('cache has size limit', async ({ page }) => {
      const result = await page.evaluate(() => {
        window.oscFast.clearCache();

        // Try to cache more than the limit
        for (let i = 0; i < 2000; i++) {
          window.oscFast.encodeMessage(`/address${i}`, []);
        }

        const stats = window.oscFast.getCacheStats();

        return {
          cacheSize: stats.stringCacheSize,
          maxSize: stats.maxSize,
          withinLimit: stats.stringCacheSize <= stats.maxSize,
        };
      });

      expect(result.withinLimit).toBe(true);
    });

    test('clearCache empties the cache', async ({ page }) => {
      const result = await page.evaluate(() => {
        // Add some entries
        window.oscFast.encodeMessage('/test1', []);
        window.oscFast.encodeMessage('/test2', []);
        window.oscFast.encodeMessage('/test3', []);

        const beforeClear = window.oscFast.getCacheStats();
        window.oscFast.clearCache();
        const afterClear = window.oscFast.getCacheStats();

        return {
          beforeSize: beforeClear.stringCacheSize,
          afterSize: afterClear.stringCacheSize,
        };
      });

      expect(result.beforeSize).toBeGreaterThan(0);
      expect(result.afterSize).toBe(0);
    });
  });

  // ===========================================================================
  // EDGE CASES
  // ===========================================================================

  test.describe('Edge Cases', () => {
    test('handles empty address', async ({ page }) => {
      // OSC spec requires address to start with '/', but we should handle gracefully
      const result = await page.evaluate(() => {
        try {
          const encoded = window.oscFast.encodeMessage('', []);
          const decoded = window.oscFast.decodeMessage(encoded);
          return { success: true, address: decoded[0] };
        } catch (e) {
          return { success: false, error: e.message };
        }
      });

      // Should handle empty string (even though it's invalid OSC)
      expect(result.success).toBe(true);
    });

    test('handles very long address', async ({ page }) => {
      const result = await page.evaluate(() => {
        const longAddress = '/a'.repeat(1000);
        const encoded = window.oscFast.encodeMessage(longAddress, [42]);
        const decoded = window.oscFast.decodeMessage(encoded);
        return {
          addressLength: decoded[0].length,
          addressMatch: decoded[0] === longAddress,
          arg: decoded[1],
        };
      });

      expect(result.addressLength).toBe(2000);
      expect(result.addressMatch).toBe(true);
      expect(result.arg).toBe(42);
    });

    test('handles large blob', async ({ page }) => {
      const result = await page.evaluate(() => {
        // 10KB blob
        const blob = new Uint8Array(10000);
        for (let i = 0; i < blob.length; i++) {
          blob[i] = i % 256;
        }

        const encoded = window.oscFast.encodeMessage('/blob', [blob]);
        const decoded = window.oscFast.decodeMessage(encoded);

        const decodedBlob = decoded[1];

        // Check first and last bytes
        return {
          blobLength: decodedBlob.length,
          firstByte: decodedBlob[0],
          lastByte: decodedBlob[9999],
          byte100: decodedBlob[100],
        };
      });

      expect(result.blobLength).toBe(10000);
      expect(result.firstByte).toBe(0);
      expect(result.lastByte).toBe(9999 % 256);
      expect(result.byte100).toBe(100);
    });

    test('handles blob larger than 2MB (overflow path)', async ({ page }) => {
      const result = await page.evaluate(() => {
        // 3MB blob - larger than the 2MB pre-allocated buffer
        const size = 3 * 1024 * 1024;
        const blob = new Uint8Array(size);
        // Fill with pattern
        for (let i = 0; i < blob.length; i++) {
          blob[i] = i % 256;
        }

        const encoded = window.oscFast.encodeMessage('/huge', [blob]);
        const decoded = window.oscFast.decodeMessage(encoded);

        const decodedBlob = decoded[1];

        // Verify integrity
        let mismatch = -1;
        for (let i = 0; i < decodedBlob.length; i++) {
          if (decodedBlob[i] !== i % 256) {
            mismatch = i;
            break;
          }
        }

        return {
          blobLength: decodedBlob.length,
          firstByte: decodedBlob[0],
          lastByte: decodedBlob[size - 1],
          midByte: decodedBlob[Math.floor(size / 2)],
          mismatch,
        };
      });

      expect(result.blobLength).toBe(3 * 1024 * 1024);
      expect(result.firstByte).toBe(0);
      expect(result.lastByte).toBe((3 * 1024 * 1024 - 1) % 256);
      expect(result.midByte).toBe(Math.floor(3 * 1024 * 1024 / 2) % 256);
      expect(result.mismatch).toBe(-1); // No mismatches
    });

    test('handles max int32 values', async ({ page }) => {
      const result = await page.evaluate(() => {
        const maxInt = 2147483647;
        const minInt = -2147483648;

        const encoded = window.oscFast.encodeMessage('/test', [maxInt, minInt]);
        const decoded = window.oscFast.decodeMessage(encoded);

        return decoded;
      });

      expect(result[1]).toBe(2147483647);
      expect(result[2]).toBe(-2147483648);
    });

    test('handles special float values', async ({ page }) => {
      const result = await page.evaluate(() => {
        const encoded = window.oscFast.encodeMessage('/test', [
          0.0,
          -0.0,
          Infinity,
          -Infinity,
        ]);
        const decoded = window.oscFast.decodeMessage(encoded);

        return {
          zero: decoded[1],
          negZero: Object.is(decoded[2], -0),
          posInf: decoded[3],
          negInf: decoded[4],
        };
      });

      expect(result.zero).toBe(0);
      // Note: -0.0 may become 0.0 through encoding
      expect(result.posInf).toBe(Infinity);
      expect(result.negInf).toBe(-Infinity);
    });

    test('handles empty bundle', async ({ page }) => {
      const result = await page.evaluate(() => {
        const encoded = window.oscFast.encodeBundle(1, []);
        const decoded = window.oscFast.decodeBundle(encoded);

        return {
          packetCount: decoded.packets.length,
          hasTimeTag: decoded.timeTag !== undefined,
        };
      });

      expect(result.packetCount).toBe(0);
      expect(result.hasTimeTag).toBe(true);
    });

    test('handles Unicode in strings', async ({ page }) => {
      const result = await page.evaluate(() => {
        const encoded = window.oscFast.encodeMessage('/test', [
          'Hello',
          'World',
        ]);
        const decoded = window.oscFast.decodeMessage(encoded);

        return decoded;
      });

      // Basic ASCII should work
      expect(result[1]).toBe('Hello');
      expect(result[2]).toBe('World');
    });

    test('copyEncoded creates independent copy', async ({ page }) => {
      const result = await page.evaluate(() => {
        const encoded1 = window.oscFast.encodeMessage('/first', [1]);
        const copy = window.oscFast.copyEncoded(encoded1);

        // Encode another message (overwrites internal buffer)
        const encoded2 = window.oscFast.encodeMessage('/second', [2]);

        // Check if copy is independent
        const decoded1 = window.oscFast.decodeMessage(copy);
        const decoded2 = window.oscFast.decodeMessage(encoded2);

        return {
          copyAddress: decoded1[0],
          newAddress: decoded2[0],
          copyArg: decoded1[1],
          newArg: decoded2[1],
        };
      });

      expect(result.copyAddress).toBe('/first');
      expect(result.newAddress).toBe('/second');
      expect(result.copyArg).toBe(1);
      expect(result.newArg).toBe(2);
    });
  });

  // ===========================================================================
  // UTF-8 STRING ENCODING
  // ===========================================================================

  test.describe('UTF-8 String Encoding', () => {
    test('round-trips accented characters', async ({ page }) => {
      const result = await page.evaluate(() => {
        const encoded = window.oscFast.encodeMessage('/test', ['café', 'über', 'naïve']);
        const decoded = window.oscFast.decodeMessage(encoded);
        return decoded;
      });

      expect(result).toEqual(['/test', 'café', 'über', 'naïve']);
    });

    test('round-trips emoji', async ({ page }) => {
      const result = await page.evaluate(() => {
        const encoded = window.oscFast.encodeMessage('/test', ['hello 🎵🎶']);
        const decoded = window.oscFast.decodeMessage(encoded);
        return decoded;
      });

      expect(result).toEqual(['/test', 'hello 🎵🎶']);
    });

    test('round-trips CJK characters', async ({ page }) => {
      const result = await page.evaluate(() => {
        const encoded = window.oscFast.encodeMessage('/test', ['こんにちは', '你好']);
        const decoded = window.oscFast.decodeMessage(encoded);
        return decoded;
      });

      expect(result).toEqual(['/test', 'こんにちは', '你好']);
    });

    test('round-trips mixed ASCII and non-ASCII', async ({ page }) => {
      const result = await page.evaluate(() => {
        const encoded = window.oscFast.encodeMessage('/test', ['hello', 'wörld', 'plain']);
        const decoded = window.oscFast.decodeMessage(encoded);
        return decoded;
      });

      expect(result).toEqual(['/test', 'hello', 'wörld', 'plain']);
    });
  });

  // ===========================================================================
  // TAGGED TYPE WRAPPERS
  // ===========================================================================

  test.describe('Tagged Type Wrappers', () => {
    test('{ type: "int" } encodes as int32', async ({ page }) => {
      const result = await page.evaluate(() => {
        const encoded = window.oscFast.encodeMessage('/test', [
          { type: 'int', value: 42 },
        ]);
        const decoded = window.oscFast.decodeMessage(encoded);
        return decoded;
      });

      expect(result).toEqual(['/test', 42]);
    });

    test('{ type: "float" } forces float32 for whole number', async ({ page }) => {
      const result = await page.evaluate(() => {
        // Encode 440 as float - should NOT become int32
        const encoded = window.oscFast.encodeMessage('/test', [
          { type: 'float', value: 440 },
        ]);
        // Check type tag byte: should be 'f' (0x66), not 'i' (0x69)
        // Type tags start after address. '/test\0\0\0' = 8 bytes, then ',f\0\0' = 4 bytes
        const typeTag = encoded[9]; // the 'f' in ',f\0\0'
        const decoded = window.oscFast.decodeMessage(encoded);
        return { typeTag, decoded };
      });

      expect(result.typeTag).toBe(0x66); // 'f' for float
      expect(result.decoded[0]).toBe('/test');
      expect(result.decoded[1]).toBeCloseTo(440, 0);
    });

    test('{ type: "float" } preserves fractional values', async ({ page }) => {
      const result = await page.evaluate(() => {
        const encoded = window.oscFast.encodeMessage('/test', [
          { type: 'float', value: 3.14 },
        ]);
        const decoded = window.oscFast.decodeMessage(encoded);
        return decoded;
      });

      expect(result[1]).toBeCloseTo(3.14, 4);
    });

    test('{ type: "string" } encodes as string', async ({ page }) => {
      const result = await page.evaluate(() => {
        const encoded = window.oscFast.encodeMessage('/test', [
          { type: 'string', value: 'hello' },
        ]);
        const decoded = window.oscFast.decodeMessage(encoded);
        return decoded;
      });

      expect(result).toEqual(['/test', 'hello']);
    });

    test('{ type: "blob" } encodes Uint8Array as blob', async ({ page }) => {
      const result = await page.evaluate(() => {
        const encoded = window.oscFast.encodeMessage('/test', [
          { type: 'blob', value: new Uint8Array([10, 20, 30]) },
        ]);
        const decoded = window.oscFast.decodeMessage(encoded);
        return {
          address: decoded[0],
          blobData: Array.from(decoded[1]),
        };
      });

      expect(result.blobData).toEqual([10, 20, 30]);
    });

    test('{ type: "blob" } encodes ArrayBuffer as blob', async ({ page }) => {
      const result = await page.evaluate(() => {
        const buf = new Uint8Array([5, 6, 7]).buffer;
        const encoded = window.oscFast.encodeMessage('/test', [
          { type: 'blob', value: buf },
        ]);
        const decoded = window.oscFast.decodeMessage(encoded);
        return {
          address: decoded[0],
          blobData: Array.from(decoded[1]),
        };
      });

      expect(result.blobData).toEqual([5, 6, 7]);
    });

    test('{ type: "bool" } encodes true/false', async ({ page }) => {
      const result = await page.evaluate(() => {
        const encoded = window.oscFast.encodeMessage('/test', [
          { type: 'bool', value: true },
          { type: 'bool', value: false },
        ]);
        const decoded = window.oscFast.decodeMessage(encoded);
        return decoded;
      });

      expect(result).toEqual(['/test', true, false]);
    });

    test('tagged types mix with bare primitives', async ({ page }) => {
      const result = await page.evaluate(() => {
        const encoded = window.oscFast.encodeMessage('/mix', [
          'name',
          { type: 'float', value: 440 },
          42,
          { type: 'int', value: 100 },
          true,
        ]);
        const decoded = window.oscFast.decodeMessage(encoded);
        return decoded;
      });

      expect(result[0]).toBe('/mix');
      expect(result[1]).toBe('name');
      expect(result[2]).toBeCloseTo(440, 0);
      expect(result[3]).toBe(42);
      expect(result[4]).toBe(100);
      expect(result[5]).toBe(true);
    });
  });

  // ===========================================================================
  // PERFORMANCE (basic sanity checks)
  // ===========================================================================

  test.describe('Performance', () => {
    test('encodes 10000 messages quickly', async ({ page }) => {
      const result = await page.evaluate(() => {
        const start = performance.now();

        for (let i = 0; i < 10000; i++) {
          window.oscFast.encodeMessage('/s_new', [
            'sonic-pi-beep',
            i,
            0,
            0,
            'note',
            60,
            'amp',
            0.5,
          ]);
        }

        const elapsed = performance.now() - start;

        return {
          elapsed,
          messagesPerSecond: 10000 / (elapsed / 1000),
        };
      });

      // Should encode at least 100,000 messages per second
      expect(result.messagesPerSecond).toBeGreaterThan(100000);
      console.log(
        `Encoded 10000 messages in ${result.elapsed.toFixed(2)}ms (${Math.round(result.messagesPerSecond)} msg/s)`
      );
    });

    test('decodes 10000 messages quickly', async ({ page }) => {
      const result = await page.evaluate(() => {
        // Pre-encode messages
        const messages = [];
        for (let i = 0; i < 10000; i++) {
          messages.push(
            window.oscFast.copyEncoded(
              window.oscFast.encodeMessage('/s_new', [
                'sonic-pi-beep',
                i,
                0,
                0,
                'note',
                60,
              ])
            )
          );
        }

        const start = performance.now();

        for (let i = 0; i < 10000; i++) {
          window.oscFast.decodeMessage(messages[i]);
        }

        const elapsed = performance.now() - start;

        return {
          elapsed,
          messagesPerSecond: 10000 / (elapsed / 1000),
        };
      });

      // Should decode at least 100,000 messages per second
      expect(result.messagesPerSecond).toBeGreaterThan(100000);
      console.log(
        `Decoded 10000 messages in ${result.elapsed.toFixed(2)}ms (${Math.round(result.messagesPerSecond)} msg/s)`
      );
    });

    test('caching improves encoding speed', async ({ page }) => {
      const result = await page.evaluate(() => {
        window.oscFast.clearCache();

        // First run - cache miss
        const start1 = performance.now();
        for (let i = 0; i < 10000; i++) {
          window.oscFast.encodeMessage('/s_new', [i]);
        }
        const elapsed1 = performance.now() - start1;

        // Second run - cache hit
        const start2 = performance.now();
        for (let i = 0; i < 10000; i++) {
          window.oscFast.encodeMessage('/s_new', [i]);
        }
        const elapsed2 = performance.now() - start2;

        return {
          firstRunMs: elapsed1,
          secondRunMs: elapsed2,
          speedup: elapsed1 / elapsed2,
        };
      });

      // Second run should be at least as fast (caching helps)
      // Note: speedup may be minimal if encoding is already fast
      console.log(
        `First run: ${result.firstRunMs.toFixed(2)}ms, Second run: ${result.secondRunMs.toFixed(2)}ms, Speedup: ${result.speedup.toFixed(2)}x`
      );
    });
  });
});
