# SuperSmoothy changes vs pristine JUCE 7.0.12

Newest first. Every entry: what changed, why, and which files.

## 2026-08-05 — WASAPI lifecycle fencing (same defect class as CoreAudio)

`juce_audio_devices/native/juce_WASAPI_windows.cpp` +
`juce_LiveObjectRegistry.h` (new — the fence extracted from the CoreAudio
fix below so both backends share it; #included inside each backend's
class scope, no module-header wiring). COMPILE-UNVERIFIED at time of
writing (authored on macOS); CI's Windows leg arbitrates.

- `SessionEventCallback::OnStateChanged` / `OnSessionDisconnected`: fenced
  on the owning `WASAPIDeviceBase` — COM session events arrive on MTA
  threads and `UnregisterAudioSessionNotification` does not drain
  in-flight callbacks. Registered in `createSessionEventCallback`,
  deregistered FIRST in `deleteSessionEventCallback`.
- `ChangeNotificationClient::notify()`: fenced on the
  `WASAPIAudioIODeviceType` — the `WeakReference` null-check alone is not
  atomic against the type's destruction (device-manager rebuilds destroy
  types). Type registers in its ctor, deregisters first in its dtor.

## 2026-08-05 — CoreAudio lifecycle + buffer-capacity hardening (#3555/#3554)

All in `juce_audio_devices/native/juce_CoreAudio_mac.cpp`. Root causes found
while debugging the Sonic Pi RC7 Bluetooth-default boot crash; the heap
overflow was confirmed with ASan, the teardown races with crash backtraces
from two machines.

- **Callback frame clamp** (`CoreAudioInternal::audioCallback`): compute the
  frame count per callback from the buffer lists' `mDataByteSize`, clamped to
  the allocated temp-buffer capacity (`tempBufferSamples`, new member set in
  `allocateTempBuffers`). Pristine code looped `bufferSize` frames, which
  overflows both our temp buffers and the HAL's buffers whenever the device
  delivers a different frame count than negotiated (aggregate with a
  renegotiated Bluetooth HFP sub-device) or `bufferSize` disagrees with the
  allocation (see next item). ASan: WRITE of 4096 into a 3888-byte block,
  every callback.
- **Reopen bodge re-allocation** (`CoreAudioInternal::reopen`): the "bodge"
  that trusts the requested rate/buffer before the device reports it now
  re-runs `allocateTempBuffers()` under `callbackLock`, so capacity always
  covers the trusted `bufferSize`. Previously `updateDetailsFromDevice`
  allocated for the *reported* size (e.g. 320) and the bodge then set
  `bufferSize` to the *requested* size (e.g. 1024) with no re-allocation.
- **Listener lifecycle fence** (`LiveInternalRegistry`, new): HAL property
  listeners fire on CoreAudio's dispatch threads and
  `AudioObjectRemovePropertyListener` does not drain in-flight callbacks.
  All four listener bodies (`deviceListenerProc`, both
  `hardwareListenerProc`s, type-level updates) now run under a registry
  lock and bail if their target instance is deregistered; deregistration
  takes the same lock, so teardown blocks until in-flight bodies complete.
  Registered: `CoreAudioInternal`, `CoreAudioIODeviceType`.
- **Teardown ordering**: `~CoreAudioIODevice` and `~AudioIODeviceCombiner`
  detach HAL listeners (new `detachHardwareListener` /
  `CoreAudioInternal::detachListener`) BEFORE closing, so no
  `restart()`/`restartAsync()` can land on a half-destroyed device or
  combiner (the RC6 #3554 SIGSEGV: `restartAsync → close` on a dying
  combiner).
- **Timer fences**: all three deferred-restart/debounce `timerCallback`s
  (`CoreAudioInternal`, `CoreAudioIODevice`, `AudioIODeviceCombiner`) run
  under the same registry fence — they fire on the message thread while an
  embedder may destroy devices on another thread (JUCE's usual
  message-thread-destruction discipline doesn't hold in SuperSonic).
  `CoreAudioIODevice`'s fences on its internal; the combiner registers
  itself.
- **Deferred-restart liveness check** (`CoreAudioIODevice::timerCallback`):
  honour `updateDetailsFromDevice()`'s existing dead-device report instead
  of reopening a device CoreAudio has invalidated (observed SIGSEGV inside
  `open()` after an aggregate vanished mid-churn); notifies
  `audioDeviceListChanged` and stays closed.

## 2026-08-04 — initial vendoring

- Copied `juce_core`, `juce_events`, `juce_audio_basics`, `juce_audio_devices`
  from JUCE tag 7.0.12 (all ISC).
- Pruned Android-only glue: `juce_core/native/java*`,
  `juce_audio_devices/native/java`, oboe.
- No source-code modifications yet.
