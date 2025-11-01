# Binary SynthDef Loading

SuperSonic loads binary `.scsyndef` files directly from the filesystem/server.

## Loading Binary SynthDefs

Load `.scsyndef` files directly using the SuperSonic API:

```javascript
import { SuperSonic } from './dist/supersonic.js';

const sonic = new SuperSonic();
await sonic.init();

// Load a single synthdef
await sonic.loadSynthDef('./dist/extra/synthdefs/sonic-pi-beep.scsyndef');

// Load multiple synthdefs
const results = await sonic.loadSynthDefs([
    'sonic-pi-beep',
    'sonic-pi-tb303',
    'sonic-pi-dsaw'
], './dist/extra/synthdefs/');
```

This is the clean, standard approach for loading synthdefs in production.

## Build System

The build script (`build.sh`) automatically:

1. Copies all `.scsyndef` files from `extra/synthdefs/` (source) to `dist/extra/synthdefs/` (build output)
2. Generates `dist/extra/synthdefs/manifest.json` with a list of all available synthdefs (120 files)

The 120 synthdefs are included in the repository (sourced from Sonic Pi) so anyone cloning the repo can build and run the examples immediately.

## File Structure

```
dist/
├── supersonic.js                    # Main library
├── wasm/
│   └── scsynth-nrt.wasm            # WASM engine
├── workers/
│   └── ...                          # Worker threads
└── extra/
    └── synthdefs/
        ├── manifest.json            # List of available synthdefs
        ├── sonic-pi-beep.scsyndef
        ├── sonic-pi-tb303.scsyndef
        └── ... (120 total)
```

## How It Works

1. Binary `.scsyndef` files are SuperCollider's native synthdef format
2. The SuperSonic API fetches them via `fetch()` and wraps them in `/d_recv` OSC messages
3. The OSC layer automatically detects binary blobs and sends them with proper encoding
4. scsynth receives and loads them natively

## Testing

Run the test page:

```bash
# Serve the project (e.g., with Python)
python3 -m http.server 8000

# Open in browser
open http://localhost:8000/test_synthdef_loading.html
```

The test demonstrates:
- Loading single synthdefs
- Loading multiple synthdefs in parallel
- Playing test notes with loaded synthdefs

## API Reference

### `SuperSonic.loadSynthDef(path)`

Load a single binary synthdef file.

**Parameters:**
- `path` (string): Path or URL to the `.scsyndef` file

**Returns:** `Promise<void>`

**Throws:** Error if file not found or load fails

### `SuperSonic.loadSynthDefs(names, baseUrl)`

Load multiple synthdefs in parallel.

**Parameters:**
- `names` (string[]): Array of synthdef names (without `.scsyndef` extension)
- `baseUrl` (string): Base URL for synthdef files (default: `'./extra/synthdefs/'`)

**Returns:** `Promise<Object>` - Map of name → `{success: boolean, error?: string}`

## Implementation Details

### New Files

- `test_synthdef_loading.html` - Test/example page

### Modified Files

- `js/supersonic.js` - Added `loadSynthDef()` and `loadSynthDefs()` methods
- `build.sh` - Copies binary synthdefs and generates manifest
- `example/assets/app.js` - Updated to use binary loading API
