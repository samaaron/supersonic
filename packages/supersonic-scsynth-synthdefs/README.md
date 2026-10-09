# supersonic-scsynth-synthdefs

Sonic Pi's synthdefs as binary `.scsyndef` files, plus a few test and utility synthdefs the SuperSonic test suite loads, for [SuperSonic](https://github.com/samaaron/supersonic).

## Installation

```bash
npm install supersonic-scsynth-synthdefs
```

Use with the client and the engine:

```bash
npm install supersonic-scsynth supersonic-scsynth-core supersonic-scsynth-synthdefs
```

## Usage

```javascript
import { SuperSonic } from 'https://unpkg.com/supersonic-scsynth@latest/dist/supersonic.js';

const supersonic = new SuperSonic({
  baseURL: 'https://unpkg.com/supersonic-scsynth@latest/dist/',
  coreBaseURL: 'https://unpkg.com/supersonic-scsynth-core@latest/',
  synthdefBaseURL: 'https://unpkg.com/supersonic-scsynth-synthdefs@latest/synthdefs/'
});

// Browsers start audio only after a click, tap or keypress
document.querySelector('button').onclick = async () => {
  await supersonic.init();

  // Load synthdefs from CDN
  await supersonic.loadSynthDefs(['sonic-pi-beep', 'sonic-pi-tb303', 'sonic-pi-prophet']);
};
```

`synthdefBaseURL` is where synthdefs loaded by name are fetched from: `loadSynthDef('sonic-pi-beep')` fetches `sonic-pi-beep.scsyndef` from there.

The package itself exports every name and, as `SYNTHDEFS_CDN`, this release's synthdefs on unpkg, from a browser or anywhere else:

```javascript
import { SYNTHDEFS_CDN, SYNTHDEF_NAMES } from 'supersonic-scsynth-synthdefs';
```

### From Node

Under Node it also exports the directory the files are in, and the path of one:

```javascript
import { SYNTHDEFS_DIR, getSynthDefPath, SYNTHDEF_NAMES } from 'supersonic-scsynth-synthdefs';

getSynthDefPath('sonic-pi-beep');   // <SYNTHDEFS_DIR>/sonic-pi-beep.scsyndef
```

## Included Synthdefs

Sonic Pi's synthdefs, including:

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
- pluck, kalimba, rhodey
- piano (silent until it is given its sample table with `/supersonic/piano/wavetable`)

### Effects (fx_*)
All standard effects:
- Reverb: reverb, gverb
- Delay: echo, ping_pong
- Filters: lpf, hpf, bpf, rbpf, nrlpf, nhpf, etc.
- Modulation: flanger, tremolo, wobble, ring_mod
- Distortion: distortion, bitcrusher, krush, tanh
- Dynamics: compressor, normaliser
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

`SYNTHDEF_NAMES`, exported by `index.js`, lists them all, and the test synthdefs too.

## Source

These synthdefs are from [Sonic Pi](https://sonic-pi.net/) by Sam Aaron.

**Source**: https://github.com/sonic-pi-net/sonic-pi/tree/dev/etc/synthdefs/compiled

## License

MIT - See [LICENSE](./LICENSE)
