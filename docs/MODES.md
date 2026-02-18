# Internal Communication Modes

> **Note**: This document describes the difference between two internal communication modes. If you're just starting out, you can ignore this and use SuperSonic without even knowing that the modes exist. Come back and read this when you want to understand the performance characteristics, deploy to production, or troubleshoot latency issues.

## SAB and PM

SuperSonic supports two modes: **PM** and **SAB**. Both modes are first-class citizens and are fully supported and tested.

* **PM Mode** _(Default)_.
  - Can be hosted on a CDN (unpkg, jsDelivr, etc.)
  - Perfect for getting started
  - Good performance
  - Full access to a regularly updated snapshot of the aggregated metrics and scsynth node-tree.
* **SAB Mode**
  - Must be self-hosted
  - Highest performance and lowest latency and jitter
  - Requires specific COOP/COEP HTTP headers
  - Browser must run in a higher security mode which introduces some restrictions regarding running external JS.
  - Full access to instant live updated aggregation of the metrics and the scsynth node-tree. No snapshots.

## Implementation Differences

### postMessage Mode

In postMessage (PM) mode, all communication between threads uses the standard [postMessage API](https://developer.mozilla.org/en-US/docs/Web/API/MessagePort/postMessage).

This mode works everywhere because postMessage is universally supported. The trade-off is that message passing has inherent overhead - each postMessage involves serialisation, event loop scheduling, and deserialisation. Overloading the main thread can also have a negative effect on postMessage delivery times.

### SAB Mode

In SAB mode, all OSC messages in and out of scsynth are transported via a ring buffer. This ring buffer exists within a [SharedArrayBuffer](https://developer.mozilla.org/en-US/docs/Web/JavaScript/Reference/Global_Objects/SharedArrayBuffer) - a region of memory that all threads can read from and write to directly.

OSC messages to scsynth go straight into the SAB if they are immediate messages or OSC Bundles (with future NTP timestamps) scheduled for the very near future. OSC Bundles for the far future go to a special holding bay called the prescheduler. All OSC bundles sent to the prescheduler are sent via postMessage (they are not in a hurry). The prescheduler ensures that the OSC Bundles get placed into the SAB ring buffer just ahead of their scheduled time.

This provides lower latency and more consistent timing because there's no serialisation or event loop scheduling overhead on the path to scsynth. However, SharedArrayBuffer requires specific security headers due to [Spectre vulnerability](https://en.wikipedia.org/wiki/Spectre_(security_vulnerability)) mitigations.

## Configuration

### postMessage Mode (Default)

No configuration needed - just create a SuperSonic instance:

```javascript
const sonic = new SuperSonic({
  baseURL: 'https://unpkg.com/supersonic-scsynth@latest/dist/'
});
```

Or explicitly specify the mode:

```javascript
const sonic = new SuperSonic({
  baseURL: '/assets/supersonic/',
  mode: 'postMessage'
});
```

### SAB Mode

Specify `mode: 'sab'` in the constructor:

```javascript
const sonic = new SuperSonic({
  baseURL: '/assets/supersonic/',
  mode: 'sab'
});
```

If the required headers are not present, `init()` will throw an error.

## Server Headers for SAB Mode

SAB mode requires your server to send two HTTP headers on all responses:

```
Cross-Origin-Opener-Policy: same-origin
Cross-Origin-Embedder-Policy: require-corp
```

These headers enable [cross-origin isolation](https://web.dev/articles/coop-coep), which is required for SharedArrayBuffer to be available.

### Server Configuration Examples

#### npx serve

Create a `serve.json` file:

```json
{
  "headers": [
    {
      "source": "**/*",
      "headers": [
        { "key": "Cross-Origin-Opener-Policy", "value": "same-origin" },
        { "key": "Cross-Origin-Embedder-Policy", "value": "require-corp" }
      ]
    }
  ]
}
```

Then run:
```bash
npx serve
```

#### Nginx

```nginx
server {
    location / {
        add_header Cross-Origin-Opener-Policy same-origin;
        add_header Cross-Origin-Embedder-Policy require-corp;
    }
}
```

#### Apache

```apache
<IfModule mod_headers.c>
    Header set Cross-Origin-Opener-Policy "same-origin"
    Header set Cross-Origin-Embedder-Policy "require-corp"
</IfModule>
```

#### Express (Node.js)

```javascript
app.use((req, res, next) => {
  res.setHeader('Cross-Origin-Opener-Policy', 'same-origin');
  res.setHeader('Cross-Origin-Embedder-Policy', 'require-corp');
  next();
});
```

#### Vite

In `vite.config.js`:

```javascript
export default {
  server: {
    headers: {
      'Cross-Origin-Opener-Policy': 'same-origin',
      'Cross-Origin-Embedder-Policy': 'require-corp'
    }
  }
}
```

## Hybrid Approach

You can use SAB mode for low-latency communication while still loading samples and synthdefs from a CDN. This gives you the best of both worlds:

```javascript
const sonic = new SuperSonic({
  // Local core (enables SAB mode with proper headers)
  baseURL: '/assets/supersonic/',
  mode: 'sab',
  // CDN for large assets
  sampleBaseURL: 'https://unpkg.com/supersonic-scsynth-samples@latest/samples/',
  synthdefBaseURL: 'https://unpkg.com/supersonic-scsynth-synthdefs@latest/synthdefs/',
});
```

See `example/hybrid.html` for a complete example.

## Technical Details

### Ring Buffer Coordination (SAB Mode)

The SAB ring buffer uses a CAS mutex for coordination. Multiple producers (main thread, workers) can write to the buffer, and a single consumer (audio worklet) reads from it. Lock acquisition has two phases: a brief `compareExchange` spin (avoids a kernel round-trip in uncontended cases), then `Atomics.wait()` which sleeps in the OS scheduler until woken by `Atomics.notify()`.

**Lock contention handling:**
- Workers use both phases: a brief CAS spin, then `Atomics.wait()` for guaranteed acquisition
- The main thread cannot call `Atomics.wait()` (a browser restriction), so it uses an optimistic approach: a single CAS attempt, and if the lock isn't immediately available, fall back to sending via the prescheduler worker (which receives messages via postMessage, so the fallback is always non-blocking)

(All messages are guaranteed to be delivered - the `ringBufferDirectWriteFails` metric tracks how often the main thread falls back to the prescheduler path (this is not an error condition).)

### Metrics Collection

Both modes support the same metrics, but collection differs:

- **SAB mode**: Metrics are written directly to a shared memory region. Reading metrics is a cheap `Atomics.load()` from shared memory.
- **postMessage mode**: Aggregated snapshots of the metrics are sent to the main thread periodically (default: every 150ms). Reading metrics accesses this cached snapshot.

In both modes, calling `getMetrics()` is cheap and safe for high-frequency use (e.g., in `requestAnimationFrame`).


## Troubleshooting

### "SharedArrayBuffer is not defined"

This error occurs when SAB mode is requested but the required headers are missing. Solutions:

1. Switch to postMessage mode: `mode: 'postMessage'`
2. Configure your server to send COOP/COEP headers (see above)
3. Check that all resources are served with the correct headers

### "init() failed with SAB mode"

Verify that:
1. Both headers are present: `Cross-Origin-Opener-Policy` and `Cross-Origin-Embedder-Policy`
2. Headers are sent on all responses (HTML, JS, WASM)
3. You're not loading cross-origin resources without proper CORS headers

### Checking if SAB is available

You can check if SharedArrayBuffer is available before initialising:

```javascript
const sabAvailable = typeof SharedArrayBuffer !== 'undefined';
console.log('SAB available:', sabAvailable);
```

### Browser DevTools

In Chrome DevTools, you can verify cross-origin isolation:
1. Open DevTools (F12)
2. Go to Application tab
3. Check "Cross-Origin Isolated" under Security
