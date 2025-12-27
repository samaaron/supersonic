# CDN Usage

SuperSonic works directly from CDN with zero configuration.

## Zero-Config CDN

The simplest way to use SuperSonic - just import and go:

```html
<script type="module">
  import { SuperSonic } from "https://unpkg.com/supersonic-scsynth@latest";

  document.getElementById('play').onclick = async () => {
    const supersonic = new SuperSonic();
    await supersonic.init();
    await supersonic.loadSynthDef("sonic-pi-beep");
    supersonic.send("/s_new", "sonic-pi-beep", -1, 0, 0, "note", 60);
  };
</script>
```

All URLs are auto-detected from the import path:
- Workers load from the same CDN path
- Synthdefs load from `supersonic-scsynth-synthdefs` package
- Samples load from `supersonic-scsynth-samples` package

No server setup, no COOP/COEP headers, no configuration needed.

## How It Works

SuperSonic uses two techniques to work from CDN:

1. **PostMessage Mode** (default) - Uses MessagePort instead of SharedArrayBuffer, avoiding the need for COOP/COEP headers

2. **Blob URL Workers** - Fetches worker scripts and creates Blob URLs, bypassing cross-origin worker restrictions

## Version Pinning

For production, pin to a specific version:

```javascript
import { SuperSonic } from "https://unpkg.com/supersonic-scsynth@0.21.6";
```

This ensures synthdefs and samples also load from matching version packages.

## Alternative CDNs

SuperSonic works with any npm CDN:

**unpkg (recommended)**
```javascript
import { SuperSonic } from "https://unpkg.com/supersonic-scsynth@latest";
```

**jsDelivr**
```javascript
import { SuperSonic } from "https://cdn.jsdelivr.net/npm/supersonic-scsynth@latest";
```

## Self-Hosting

For production apps, you may prefer to self-host. SuperSonic auto-detects paths from the import location:

```javascript
import { SuperSonic } from "./supersonic/supersonic.js";

const supersonic = new SuperSonic();  // Paths auto-detected
await supersonic.init();
```

Or with explicit configuration:

```javascript
const supersonic = new SuperSonic({
  baseURL: "/supersonic/"
});
```

### Self-Hosted Directory Structure

```
your-site/
├── supersonic/
│   ├── supersonic.js
│   ├── workers/
│   │   ├── scsynth_audio_worklet.js
│   │   ├── osc_in_worker.js
│   │   ├── osc_out_prescheduler_worker.js
│   │   └── debug_worker.js
│   ├── wasm/
│   │   └── scsynth-nrt.wasm
│   ├── synthdefs/     # Optional - can use CDN
│   └── samples/       # Optional - can use CDN
```

### Hybrid: Self-Host Core, CDN for Assets

Self-host the small core files, use CDN for large assets:

```javascript
const supersonic = new SuperSonic({
  baseURL: "/supersonic/",
  synthdefBaseURL: "https://unpkg.com/supersonic-scsynth-synthdefs@latest/synthdefs/",
  sampleBaseURL: "https://unpkg.com/supersonic-scsynth-samples@latest/samples/"
});
```

## SAB Mode (Advanced)

For lowest latency, use SharedArrayBuffer mode. This requires COOP/COEP headers but provides direct memory access:

```javascript
const supersonic = new SuperSonic({ mode: 'sab' });
```

See [Browser Setup](BROWSER_SETUP.md) for header configuration.

**Note:** SAB mode with CDN requires your page to have COOP/COEP headers, even though the files come from CDN.

## npm Packages

Install for bundling or local development:

```bash
npm install supersonic-scsynth           # Core library
npm install supersonic-scsynth-synthdefs # Synth definitions
npm install supersonic-scsynth-samples   # Audio samples
```
