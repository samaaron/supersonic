# SuperSmoothy changes vs pristine JUCE 7.0.12

Newest first. Every entry: what changed, why, and which files.

## 2026-08-04 — initial vendoring

- Copied `juce_core`, `juce_events`, `juce_audio_basics`, `juce_audio_devices`
  from JUCE tag 7.0.12 (all ISC).
- Pruned Android-only glue: `juce_core/native/java*`,
  `juce_audio_devices/native/java`, oboe.
- No source-code modifications yet.
