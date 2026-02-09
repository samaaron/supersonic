# SuperSonic API Improvements — NTP Timestamps & OSC Bundles

## Problem

The current `oscFast.encodeBundle(timeTag, packets)` API has several sharp edges that we hit while building the Sonic Pi Web VM:

1. **Silent failure on `[sec, frac]` arrays**: Passing `[3979582043, 1234567]` (a natural representation of an NTP timetag) silently produces timetag `[0, 0]` (immediate) because `array >>> 0` returns `0`. No error, no warning — just broken timing.

2. **Undocumented format**: Nothing indicates that `timeTag` must be a single float representing NTP seconds since 1900. Users have to read `writeTimeTag` source to discover this.

3. **NTP conversion is left to the caller**: Users must know the magic constant `2208988800`, convert `Date.now()` to seconds, add the offset, and hope they got it right. Meanwhile, SuperSonic already has `NTP_EPOCH_OFFSET`, `calculateCurrentNTP()`, and `getCurrentNTPFromPerformance()` internally — but none are exported.

4. **API inconsistency**: The backwards-compatible `SuperSonic.osc.encode(packet)` method handles `packet.timeTag.raw = [seconds, fraction]` arrays, but `encodeBundle()` does not.

## Proposed Improvements

### 1. Accept multiple timetag formats in `encodeBundle`

```javascript
// All of these should work:
oscFast.encodeBundle(ntpFloat, packets)           // current: single NTP float
oscFast.encodeBundle([ntpSec, ntpFrac], packets)  // NEW: [uint32, uint32] pair
oscFast.encodeBundle(1, packets)                  // current: immediate (timetag 0x01)
oscFast.encodeBundle(null, packets)               // NEW: immediate (explicit)
```

In `writeTimeTag`:
```javascript
function writeTimeTag(time, pos) {
  if (time === 1 || time === null || time === undefined) {
    encodeView.setUint32(pos, 0, false);
    encodeView.setUint32(pos + 4, 1, false);
    return pos + 8;
  }
  if (Array.isArray(time)) {
    encodeView.setUint32(pos, time[0] >>> 0, false);
    encodeView.setUint32(pos + 4, time[1] >>> 0, false);
    return pos + 8;
  }
  if (typeof time !== 'number') {
    throw new TypeError(`encodeBundle timeTag: expected number or [sec, frac] array, got ${typeof time}`);
  }
  const seconds = time >>> 0;
  const fraction = ((time - Math.floor(time)) * TWO_POW_32) >>> 0;
  encodeView.setUint32(pos, seconds, false);
  encodeView.setUint32(pos + 4, fraction, false);
  return pos + 8;
}
```

### 2. Export NTP conversion helpers on `SuperSonic.osc`

```javascript
SuperSonic.osc = {
  // existing...
  encodeBundle, encodeMessage, decode, encode,

  // NEW: timing helpers
  NTP_EPOCH_OFFSET: 2208988800,

  /** Convert Date.now() (or any Unix epoch ms) to NTP float for use as timetag */
  dateToNTP(dateMs) {
    return dateMs / 1000 + 2208988800;
  },

  /** Convert NTP float back to Date.now()-compatible Unix epoch ms */
  ntpToDate(ntpFloat) {
    return (ntpFloat - 2208988800) * 1000;
  },

  /** Get current NTP time (works in any context — main thread, worker, worklet) */
  now() {
    return (performance.timeOrigin + performance.now()) / 1000 + 2208988800;
  },
};
```

Usage becomes trivial:
```javascript
const { osc } = SuperSonic;
const ntpNow = osc.now();
const bundle = osc.encodeBundle(ntpNow + 0.5, [{ address: '/s_new', args }]);
```

### 3. `encodeSingleBundle` — expose publicly

Already exists internally, useful for the common case of one message per bundle:

```javascript
SuperSonic.osc.encodeSingleBundle(timeTag, address, args)
```

### 4. Higher-level scheduling helper (optional)

For users who don't want to think about NTP at all:

```javascript
// Schedule an OSC message relative to "now"
const bundle = SuperSonic.osc.scheduleBundle(
  delayMs,  // milliseconds from now (0 = immediate, 250 = 250ms in future)
  [{ address: '/s_new', args: ['sonic-pi-beep', -1, 0, 0, 'note', 60] }]
);
channel.send(bundle);
```

This combines `osc.now() + delayMs/1000` with `encodeBundle` in one call. Eliminates an entire class of bugs (wrong epoch, wrong units, wrong format).

### 5. Input validation

Currently invalid inputs produce silent, hard-to-debug failures. Add validation:

```javascript
function writeTimeTag(time, pos) {
  // Catch common mistakes early
  if (Array.isArray(time) && time.length !== 2) {
    throw new Error(`encodeBundle timeTag array must have exactly 2 elements [sec, frac], got ${time.length}`);
  }
  if (typeof time === 'number' && time > 1 && time < NTP_EPOCH_OFFSET) {
    console.warn(`encodeBundle: timeTag ${time} looks like Unix seconds, not NTP. Did you forget to add NTP_EPOCH_OFFSET (2208988800)?`);
  }
  // ...
}
```

### 6. Export `readTimetag` for debugging

Already exists in osc_classifier.js but not exposed:

```javascript
SuperSonic.osc.readTimetag(bundleData)
// Returns: { ntpSeconds: uint32, ntpFraction: uint32 } or null
```

Useful for debugging: "what timetag is actually in this bundle?"

### 7. `snapshotMetrics()` / `diffMetrics()` — see the delta

Integration tests need to isolate metrics to a specific test run. Currently every consumer has to implement the same before/after/subtract pattern:

```javascript
// What every consumer has to write today
const before = supersonic.getMetrics();
await doStuff();
const after = supersonic.getMetrics();
const delta = {};
for (const key of Object.keys(after)) {
  if (typeof after[key] === 'number') delta[key] = after[key] - before[key];
}
```

SuperSonic should own this:

```javascript
const snap = supersonic.snapshotMetrics();
await doStuff();
const diff = supersonic.diffMetrics(snap);

expect(diff.bypassLate).toBe(0);
expect(diff.scsynthSchedulerLates).toBe(0);
expect(diff.preschedulerBundlesScheduled).toBeGreaterThan(0);
```

Non-destructive (no state reset), safe for concurrent use, and the diff logic lives where the metric types are defined (SuperSonic knows which fields are numeric counters vs string enums vs objects).

## Priority

| Improvement | Impact | Effort | Priority |
|---|---|---|---|
| Accept `[sec, frac]` arrays | Prevents silent failure | Low | P0 |
| Export `osc.now()` | Eliminates NTP math for users | Low | P0 |
| Input validation / warnings | Debugging aid | Low | P1 |
| Export `osc.dateToNTP()` | Eliminates magic constant | Low | P1 |
| `snapshotMetrics()` / `diffMetrics()` | Clean test assertions | Low | P1 |
| Expose `encodeSingleBundle` | Convenience | Low | P2 |
| `scheduleBundle(delayMs, ...)` | Best DX | Medium | P2 |
| Export `readTimetag` | Debugging aid | Low | P2 |

## Before / After

**Before** (what we had to write):
```javascript
const wallClockMs = Date.now() + event.beat * msPerBeat;
const wallClockSec = wallClockMs / 1000;
const NTP_EPOCH_OFFSET = 2208988800;
const ntpTime = wallClockSec + NTP_EPOCH_OFFSET;
const bundle = oscFast.encodeBundle(ntpTime, [{ address, args }]);
```

**After** (with proposed helpers):
```javascript
const delayMs = event.beat * msPerBeat;
const bundle = SuperSonic.osc.scheduleBundle(delayMs, [{ address, args }]);
// or
const bundle = SuperSonic.osc.encodeBundle(SuperSonic.osc.now() + delayMs / 1000, [{ address, args }]);
```
