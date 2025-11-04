# supersonic-scsynth-synthdefs

All Sonic Pi synthdefs (120 binary `.scsyndef` files) for [SuperSonic](https://github.com/samaaron/supersonic).

## Installation

```bash
npm install supersonic-scsynth-synthdefs
```

Use with the core engine:

```bash
npm install supersonic-scsynth supersonic-scsynth-synthdefs
```

## Usage

### Browser with CDN

```javascript
import { SuperSonic } from 'https://unpkg.com/supersonic-scsynth';

const sonic = new SuperSonic();
await sonic.init();

// Load synthdefs from CDN
await sonic.loadSynthDefs(
  ['sonic-pi-beep', 'sonic-pi-tb303', 'sonic-pi-prophet'],
  'https://unpkg.com/supersonic-scsynth-synthdefs/synthdefs/'
);
```

### Browser with Bundler (Vite/Webpack)

Copy synthdefs to your public directory:

```bash
cp -r node_modules/supersonic-scsynth-synthdefs/synthdefs public/
```

Then use:

```javascript
import { SuperSonic } from 'supersonic-scsynth';

const sonic = new SuperSonic();
await sonic.init();

await sonic.loadSynthDefs(
  ['sonic-pi-beep', 'sonic-pi-tb303'],
  '/synthdefs/'
);
```

### Using Path Helper (Node.js)

```javascript
import { SYNTHDEFS_CDN } from 'supersonic-scsynth-synthdefs';

// Use CDN path for browser compatibility
await sonic.loadSynthDefs(['sonic-pi-beep'], SYNTHDEFS_CDN);
```

## Included Synthdefs

All 120 Sonic Pi synthdefs including:

### Synths
- Basic: beep, saw, square, tri, pulse
- Analog: dsaw, dpulse, dtri, prophet, tb303
- FM: fm, mod_fm
- Subtractive: bass_foundation, bass_highend, blade, hoover, zawa
- Chip: chiplead, chipbass, chipnoise

### Noise
- bnoise, cnoise, gnoise, pnoise

### Pads & Ambient
- dark_ambience, hollow, growl, organ_tonewheel

### Plucked & Percussion
- pluck, kalimba, piano, rhodey

### Effects (fx_*)
All standard effects:
- Reverb: reverb, gverb
- Delay: echo, ping_pong
- Filters: lpf, hpf, bpf, rbpf, nrlpf, nhpf, etc.
- Modulation: flanger, tremolo, wobble, ring_mod
- Distortion: distortion, bitcrusher, krush, tanh
- Dynamics: compressor, normaliser, limiter
- Spatial: pan, panslicer
- Spectral: pitch_shift, octaver, whammy
- And more...

### Drums (sc808_*)
Complete TR-808 drum machine:
- bassdrum, snare, rimshot
- closed_hihat, open_hihat
- clap, cowbell, maracas
- cymbal, claves
- tom (hi, mid, lo)
- conga (hi, mid, lo)

See [synthdefs/README.md](synthdefs/README.md) for complete list.

## Source

These synthdefs are from [Sonic Pi](https://sonic-pi.net/) by Sam Aaron.

**Source**: https://github.com/sonic-pi-net/sonic-pi/tree/dev/etc/synthdefs/compiled

## License

GPL-3.0-or-later
