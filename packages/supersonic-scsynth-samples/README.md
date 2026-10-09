# SuperSonic Samples

Sonic Pi's audio samples in one convenient package, as FLAC files (plus an AIFF copy of `bd_haus`, which SuperSonic's tests load).

## Installation

```bash
npm install supersonic-scsynth-samples
```

## Usage

```javascript
import { SuperSonic } from 'https://unpkg.com/supersonic-scsynth@latest/dist/supersonic.js';

const supersonic = new SuperSonic({
  baseURL: 'https://unpkg.com/supersonic-scsynth@latest/dist/',
  coreBaseURL: 'https://unpkg.com/supersonic-scsynth-core@latest/',
  synthdefBaseURL: 'https://unpkg.com/supersonic-scsynth-synthdefs@latest/synthdefs/',
  sampleBaseURL: 'https://unpkg.com/supersonic-scsynth-samples@latest/samples/'
});

// Browsers start audio only after a click, tap or keypress
document.querySelector('button').onclick = async () => {
  await supersonic.init();

  // Load any sample
  await supersonic.loadSample(0, 'bd_haus.flac');
  await supersonic.loadSample(1, 'loop_amen.flac');

  // Play it with the basic_mono_player synthdef, in the root group (0)
  await supersonic.loadSynthDef('sonic-pi-basic_mono_player');
  supersonic.send('/s_new', 'sonic-pi-basic_mono_player', -1, 0, 0, 'buf', 0);
};
```

`sampleBaseURL` is where samples named without a path are fetched from.

## Available Samples

This package includes Sonic Pi's samples, organised by category. Their names are also exported, as `SAMPLE_NAMES`, by the package, from a browser or anywhere else; under Node it exports `SAMPLES_DIR` and `getSamplePath(file)` as well.

### Ambient (11 samples)
`ambi_choir`, `ambi_dark_woosh`, `ambi_drone`, `ambi_glass_hum`, `ambi_glass_rub`, `ambi_haunted_hum`, `ambi_lunar_land`, `ambi_piano`, `ambi_sauna`, `ambi_soft_buzz`, `ambi_swoosh`

### Bass Drums (15 samples)
`bd_808`, `bd_ada`, `bd_boom`, `bd_chip`, `bd_fat`, `bd_gas`, `bd_haus`, `bd_jazz`, `bd_klub`, `bd_mehackit`, `bd_pure`, `bd_sone`, `bd_tek`, `bd_zome`, `bd_zum`

### Loops (17 samples)
`loop_3d_printer`, `loop_amen`, `loop_amen_full`, `loop_breakbeat`, `loop_compus`, `loop_drone_g_97`, `loop_electric`, `loop_garzul`, `loop_industrial`, `loop_mehackit1`, `loop_mehackit2`, `loop_mika`, `loop_perc1`, `loop_perc2`, `loop_safari`, `loop_tabla`, `loop_weirdo`

### Electronic (25 samples)
`elec_beep`, `elec_bell`, `elec_blip`, `elec_blip2`, `elec_blup`, `elec_bong`, `elec_chime`, `elec_cymbal`, `elec_filt_snare`, `elec_flip`, `elec_fuzz_tom`, `elec_hi_snare`, `elec_hollow_kick`, `elec_lo_snare`, `elec_mid_snare`, `elec_ping`, `elec_plip`, `elec_pop`, `elec_snare`, `elec_soft_kick`, `elec_tick`, `elec_triangle`, `elec_twang`, `elec_twip`, `elec_wood`

### Drums (20 samples)
`drum_bass_hard`, `drum_bass_soft`, `drum_cowbell`, `drum_cymbal_closed`, `drum_cymbal_hard`, `drum_cymbal_open`, `drum_cymbal_pedal`, `drum_cymbal_soft`, `drum_heavy_kick`, `drum_roll`, `drum_snare_hard`, `drum_snare_soft`, `drum_splash_hard`, `drum_splash_soft`, `drum_tom_hi_hard`, `drum_tom_hi_soft`, `drum_tom_lo_hard`, `drum_tom_lo_soft`, `drum_tom_mid_hard`, `drum_tom_mid_soft`

Plus: Glitch, Guitar, Hi-hats, Percussion, Snares, Tabla, Vinyl, and more!

See [PROVENANCE.md](./PROVENANCE.md) for full attribution and Freesound.org links.

## Package Size

About 35 MB, already compressed as FLAC.

Each sample is fetched only when you load it, so initial page load remains fast.

## License

CC0-1.0 (Public Domain) - See [LICENSE](./LICENSE)

All samples originally from [Sonic Pi](https://github.com/sonic-pi-net/sonic-pi).

## Related Packages

- [`supersonic-scsynth`](https://www.npmjs.com/package/supersonic-scsynth) - Client API
- [`supersonic-scsynth-core`](https://www.npmjs.com/package/supersonic-scsynth-core) - WASM engine and AudioWorklet
- [`supersonic-scsynth-synthdefs`](https://www.npmjs.com/package/supersonic-scsynth-synthdefs) - Synth definitions
- [`supersonic-scsynth-bundle`](https://www.npmjs.com/package/supersonic-scsynth-bundle) - Everything together
