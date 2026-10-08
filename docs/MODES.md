# Internal Communication Modes

> **Note**: This document describes the difference between two internal communication modes. If you're just starting out, you can ignore this and use SuperSonic without even knowing that the modes exist. Come back and read this when you want to understand the performance characteristics, deploy to production, or troubleshoot latency issues.

These modes are the browser's. For SuperSonic running natively or as a BEAM NIF, see the [Native Guide](NATIVE.md) and the [NIF Guide](NIF.md).

## SAB and PM

SuperSonic supports two modes: **PM** and **SAB**. Both modes are first-class citizens and are fully supported and tested.

SuperSonic picks one for you: SAB when the page is cross-origin isolated, PM when it is not. You can also choose (see [Configuration](#configuration)).

* **PM Mode**
  - Used when the page is not cross-origin isolated
  - Needs no special headers, so it works on any host
  - Perfect for getting started
  - Good performance
  - Full access to a regularly updated snapshot of the aggregated metrics and scsynth node-tree.
* **SAB Mode**
  - Used when the page is cross-origin isolated
  - Highest performance and lowest latency and jitter
  - Requires specific COOP/COEP HTTP headers on your page
  - Cross-origin isolation makes the browser stricter: everything the page loads from another origin must allow it (CORS, or a `Cross-Origin-Resource-Policy` header).
  - Full access to instant live updated aggregation of the metrics and the scsynth node-tree. No snapshots.
  - Scopes (`getScope()`, `getScopes()`) and audio capture (`startCapture()`) are SAB-only. In PM mode `getScope()` returns `null`, `getScopes()` returns `[]` and `startCapture()` throws.

## Implementation Differences

### postMessage Mode

In postMessage (PM) mode, all communication between threads uses the standard [postMessage API](https://developer.mozilla.org/en-US/docs/Web/API/MessagePort/postMessage).

This mode works everywhere because postMessage is universally supported. The trade-off is that message passing has inherent overhead - each postMessage involves serialisation, event loop scheduling, and deserialisation. Overloading the main thread can also have a negative effect on postMessage delivery times.

### SAB Mode

In SAB mode, all OSC messages in and out of scsynth are transported via a ring buffer. This ring buffer exists within a [SharedArrayBuffer](https://developer.mozilla.org/en-US/docs/Web/JavaScript/Reference/Global_Objects/SharedArrayBuffer) - a region of memory that all threads can read from and write to directly.

Every OSC message to scsynth goes straight into the ring buffer, bundles included, however far in the future their timestamps are. Timing is the engine's job, in both modes: its audio thread reads each message, and holds a bundle in its scheduler until the bundle's time comes.

This provides lower latency and more consistent timing because there's no serialisation or event loop scheduling overhead on the path to scsynth. However, SharedArrayBuffer requires specific security headers due to [Spectre vulnerability](https://en.wikipedia.org/wiki/Spectre_(security_vulnerability)) mitigations.

## Configuration

Without a `mode`, SuperSonic uses SAB if the page is cross-origin isolated and PM otherwise:

```javascript
const sonic = new SuperSonic({
  baseURL: '/assets/supersonic/'
});
```

`sonic.mode` tells you which one it picked.

### postMessage Mode

To use PM even on an isolated page:

```javascript
const sonic = new SuperSonic({
  baseURL: '/assets/supersonic/',
  mode: 'postMessage'
});
```

### SAB Mode

To insist on SAB:

```javascript
const sonic = new SuperSonic({
  baseURL: '/assets/supersonic/',
  mode: 'sab'
});
```

If the page is not cross-origin isolated, `init()` will throw an error.

## Server Headers for SAB Mode

SAB mode requires your server to send two HTTP headers on all responses:

```
Cross-Origin-Opener-Policy: same-origin
Cross-Origin-Embedder-Policy: require-corp
```

These headers enable [cross-origin isolation](https://web.dev/articles/coop-coep), which is required for SharedArrayBuffer to be available.

They are headers on your own server's responses. SuperSonic's files can still come from a CDN, as long as it sends CORS headers (unpkg and jsDelivr do): the client loads cross-origin workers and the AudioWorklet from blob URLs, which run with your page's isolation.

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

You can serve SuperSonic's client and engine yourself while loading samples and synthdefs from a CDN:

```javascript
const sonic = new SuperSonic({
  // Local client and engine
  baseURL: '/assets/supersonic/',
  // CDN for large assets
  sampleBaseURL: 'https://unpkg.com/supersonic-scsynth-samples@latest/samples/',
  synthdefBaseURL: 'https://unpkg.com/supersonic-scsynth-synthdefs@latest/synthdefs/',
});
```

See `example/hybrid.html` for a complete example.

## Technical Details

### Ring Buffer Coordination (SAB Mode)

Several producers - the main thread, and each worker with an [OscChannel](WORKERS.md) - write to the ring buffer, and a single consumer (the audio worklet) reads from it. Each producer writes the ring itself, running the engine's own ring code over the shared memory.

Producers take turns through a spinlock. It is held only for a message header and a copy of the message's bytes, so a producer that finds it taken waits a moment rather than giving up, and nothing calls `Atomics.wait()` to send. A send fails only when the ring is full: that message is dropped and counted in the `ringBufferDirectWriteFails` metric. There is no fallback path.

### Metrics Collection

Both modes support the same metrics, but collection differs:

- **SAB mode**: Metrics are written directly to a shared memory region. Reading metrics is a cheap `Atomics.load()` from shared memory.
- **postMessage mode**: Aggregated snapshots of the metrics are sent to the main thread periodically (default: every 150ms, set by `snapshotIntervalMs`). Reading metrics accesses this cached snapshot.

In both modes, calling `getMetrics()` is cheap and safe for high-frequency use (e.g., in `requestAnimationFrame`).


## Troubleshooting

### "Missing required features for sab mode"

This error occurs when SAB mode is requested but the page is not cross-origin isolated. Solutions:

1. Leave `mode` out, or switch to postMessage mode: `mode: 'postMessage'`
2. Configure your server to send COOP/COEP headers (see above)
3. Check that all resources are served with the correct headers

### "init() failed with SAB mode"

Verify that:
1. Both headers are present: `Cross-Origin-Opener-Policy` and `Cross-Origin-Embedder-Policy`
2. Headers are sent on all responses (HTML, JS, WASM)
3. You're not loading cross-origin resources without proper CORS headers

### Checking if the page is cross-origin isolated

SuperSonic decides with `crossOriginIsolated`, and you can check it yourself before initialising, or in the browser console:

```javascript
console.log('Cross-origin isolated:', crossOriginIsolated);
```
