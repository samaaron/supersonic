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

**Important:** SuperSonic cannot be loaded from a CDN. You must self-host the core library and serve it with COOP/COEP headers. Synthdefs can be loaded from a CDN.

### Self-hosted core with CDN synthdefs

```javascript
// Self-hosted core library
import { SuperSonic } from './dist/supersupersonic.js';

const supersonic = new SuperSonic({
  synthdefBaseURL: 'https://unpkg.com/supersonic-scsynth-synthdefs/synthdefs/'
});
await supersonic.init();

// Load synthdefs from CDN
await supersonic.loadSynthDefs(['sonic-pi-beep', 'sonic-pi-tb303', 'sonic-pi-prophet']);
```

### Self-hosted core and synthdefs

Copy synthdefs to your public directory:

```bash
cp -r node_modules/supersonic-scsynth-synthdefs/synthdefs public/
```

Then use:

```javascript
import { SuperSonic } from './dist/supersupersonic.js';

const supersonic = new SuperSonic({
  synthdefBaseURL: '/synthdefs/'
});
await supersonic.init();

await supersonic.loadSynthDefs(['sonic-pi-beep', 'sonic-pi-tb303']);
```

### Using the CDN path helper

```javascript
import { SuperSonic } from './dist/supersupersonic.js';
import { SYNTHDEFS_CDN } from 'supersonic-scsynth-synthdefs';

const supersonic = new SuperSonic({
  synthdefBaseURL: SYNTHDEFS_CDN
});
await supersonic.init();
await supersonic.loadSynthDefs(['sonic-pi-beep']);
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
