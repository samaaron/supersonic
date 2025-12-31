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

```javascript
import { SuperSonic } from 'https://unpkg.com/supersonic-scsynth@latest';

const supersonic = new SuperSonic({
  synthdefBaseURL: 'https://unpkg.com/supersonic-scsynth-synthdefs@latest/synthdefs/'
});
await supersonic.init();

// Load synthdefs from CDN
await supersonic.loadSynthDefs(['sonic-pi-beep', 'sonic-pi-tb303', 'sonic-pi-prophet']);
```

SuperSonic works directly from CDN with zero configuration.

### Using the CDN path helper

```javascript
import { SuperSonic } from 'https://unpkg.com/supersonic-scsynth@latest';
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

MIT - See [LICENSE](./LICENSE)
