# Sonic Pi SynthDefs

This directory contains 120 binary synthdef files (`.scsyndef`) from [Sonic Pi](https://sonic-pi.net/).

## About

These synthdefs are SuperCollider synth definitions compiled to binary format. They include:
- Lead synths (beep, chiplead, prophet, etc.)
- Bass synths (tb303, bass_foundation, etc.)
- Noise generators (bnoise, gnoise, pnoise, etc.)
- Effects (reverb, delay, filters, etc.)
- Drum machines (sc808 series)
- And many more!

## License

These synthdefs are part of Sonic Pi and are licensed under the GPL v3 or later.

**Source**: https://github.com/sonic-pi-net/sonic-pi/tree/dev/etc/synthdefs/compiled

## Usage

These files are automatically copied to `dist/etc/synthdefs/` during the build process and can be loaded using the SuperSonic API:

```javascript
// Load a single synthdef
await sonic.loadSynthDef('./dist/etc/synthdefs/sonic-pi-beep.scsyndef');

// Load multiple synthdefs
await sonic.loadSynthDefs(['sonic-pi-beep', 'sonic-pi-tb303']);
```

See [SYNTHDEF_LOADING.md](../../SYNTHDEF_LOADING.md) for more details.
