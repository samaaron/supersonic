# SuperSmoothy

SuperSonic's own audio-device layer: a vendored, ISC-licensed fork of the four
permissive modules of JUCE 7, which we maintain and hack on directly.

## Provenance

- Source: JUCE tag **7.0.12** (the last JUCE whose core/audio modules are ISC —
  JUCE ≥ 8 is AGPLv3/commercial, which SuperSonic's no-AGPL boundary forbids;
  see `docs/UPSTREAM_SYNC_GUIDE.md`).
- Modules: `juce_core`, `juce_events`, `juce_audio_basics`, `juce_audio_devices`
  — all ISC. `juce_audio_formats` (GPL-dual) was deliberately NOT vendored; the
  recorder uses libsndfile instead.
- Pruned relative to upstream: Android glue (`native/java*`, oboe) — SuperSmoothy
  targets macOS / Windows / Linux only.
- The `juce` namespace and per-file copyright headers are retained, as the ISC
  licence requires. SuperSmoothy is the subproject/target name, not a rename.

## Rules

1. **Never sync, backport, or transcribe code from JUCE 8 or later.** Those
   trees are AGPL-side; a retyped fragment still carries AGPL. Fixes here are
   written fresh or derived from this tree itself.
2. Every divergence from pristine 7.0.12 is logged in
   [SUPERSONIC-CHANGES.md](SUPERSONIC-CHANGES.md) (same pattern as Sonic Pi's
   QScintilla fork).
3. New files added under this tree are ISC, copyright Sam Aaron and the Sonic
   Pi Core Team.

## Build

`supersmoothy/CMakeLists.txt` builds one static library target `supersmoothy`
that compiles each module's unity source with the module-format defines and
exports `modules/` as the include root, so existing
`#include <juce_audio_devices/juce_audio_devices.h>` lines work unchanged.
