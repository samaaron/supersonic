# SuperSonic OSC API

Wire-protocol reference for the `/supersonic/*` OSC surface that
SuperSonic exposes alongside the standard scsynth OSC commands.
scsynth's own commands (`/s_new`, `/d_recv`, `/n_free`, `/status`,
etc.) are documented upstream by SuperCollider and aren't repeated
here — this file covers only the SuperSonic-specific management
extensions for device selection, recording, notifications, and clock.

> Looking for the JavaScript/WASM embedder API instead? See
> [`API.md`](API.md) (auto-generated from `supersonic.d.ts`).

## Transport

- UDP, OSC 1.0 encoding (osc-pack).
- Default port: **57110** (configurable via `Config::udpPort` /
  `--udp-port`).
- Bind address: `127.0.0.1` by default. Override with `--bind` or
  `Config::bindAddress` for LAN exposure.
- Messages only — no bundles required for any endpoint. Bundles work
  but add no semantics.
- Argument tags follow standard OSC: `i` = int32, `f` = float32,
  `s` = C string, `b` = blob.

## Conventions

### Request / reply pattern

Most inbound commands at `/supersonic/<area>/<verb>` produce an
outbound reply at `/supersonic/<area>/<verb>.reply` sent back to the
command's sender. Replies are **one-shot** unless the documentation
for a specific endpoint says otherwise (reopen emits both `.reply`
and `.done`; `devices/list` emits N `.reply` messages followed by a
`.done` terminator).

The reply's first argument is typically an `int32` success flag
(`1` = OK, `0` = error) followed by either the requested data or an
error string. See each endpoint below.

### Notify targets (push notifications)

Events that aren't responses to a specific command (`statechange`,
`setup`, `info`, `devices`, `input-devices`, `devices/reopen.done`)
are pushed to all **registered notify targets**. A client registers
itself as a target by sending `/supersonic/notify`; register as
early as possible at boot so the first `statechange` fires against
your socket.

### Sentinels

Two magic strings appear where a device name is expected:

| Sentinel | Applies to | Meaning |
| --- | --- | --- |
| `__system__` | `devices/switch` output name | Follow macOS system default output (engine enters "system" device mode). |
| `__none__` | `devices/switch` input name | Disable audio inputs. Clears the preferred input device for hot-plug. |

Anything else is treated as a literal device name. JUCE may append
`" (N)"` to disambiguate duplicate CoreAudio names; SuperSonic's
matcher accepts either the bare name or the suffixed form.

---

## Notify registration

### `→ /supersonic/notify` *(no args)*

Registers the sender's IP:port as a notify target for all push
events. On registration, SuperSonic immediately emits a full device
report (`/supersonic/devices` + `/supersonic/input-devices` +
`/supersonic/info`) so the new client starts with an accurate
picture.

**Reply:** `/supersonic/notify.reply i:1`

### `→ /supersonic/notify/unregister` *(no args)*

Removes the sender from the notify target list. Polite shutdown. No
reply.

### `→ /supersonic/notify/clear` *(no args)*

Removes **all** notify targets. Used before a full client restart so
stale targets from the previous session don't receive events meant
for the new one. No reply.

---

## Lifecycle push events

These are broadcast to every registered notify target — no request
triggers them (though registering or sending `devices/report` makes
SuperSonic emit the device set immediately).

### `← /supersonic/statechange s:state s:reason`

Engine state transition. `state` is one of:
`stopped`, `booting`, `running`, `restarting`, `error`.
`reason` is a human-readable cause (`"init"`, `"boot"`,
`"rate-change"`, `"swap-failed-rollback"`, `"swap-recovered"`,
`"shutdown"`).

### `← /supersonic/setup i:sampleRate i:bufferSize`

Emitted when the World is (re)built — i.e. after a successful cold
swap. Clients should use this signal to re-register for notifications
and rebuild their mixer graph. Not emitted for hot swaps.

### `← /supersonic/info s:banner i:sampleRate i:bufferSize`  `i:numRates f:rate1..rateN` `i:numBufs i:buf1..bufN` `i:numDrivers s:driver1..driverN` `s:currentDriver` `i:outputChannels i:inputChannels`

Hardware info message. The banner is a multi-line pre-formatted
string suitable for direct display (driver, sample rate, buffer,
latency). The structured fields after it are the same data the GUI
needs for populating dropdowns.

**Rate and buffer lists** are filtered to the canonical useful set
and, on a drift-compensated aggregate, constrained to the current
values only. **Channel counts** are per-device (not aggregate sums)
so a user who picked 2-channel MBP Speakers sees `out 2` even if the
engine is running on a `MBP + MOTU` aggregate.

### `← /supersonic/devices s:mode s:current s:device1..deviceN i:sampleRate i:compat1..compatN`

Output device list.
- `mode` — `"system"` if following the default, otherwise the
  user-selected device name.
- `current` — actual device in use right now.
- `device1..deviceN` — visible output devices (wireless transports
  are hidden from the dropdown list).
- `sampleRate` — current rate.
- `compat1..compatN` — per-device `1` if that device supports the
  current rate without a cold swap, `0` otherwise. GUI can use this
  to warn about rate changes.

### `← /supersonic/input-devices s:current i:numDevices s:device1..deviceN`

Input device list. Separate from the output message because input
and output can diverge (e.g. an aggregate device combines two).

---

## Device query

### `→ /supersonic/devices/list` *(no args)*

Returns the full device inventory. Unlike the push
`/supersonic/devices` report above (which is dropdown-focused and
hides wireless devices), this gives one reply per visible device
with detailed metadata.

**Reply stream:**
- For each device:
  `← /supersonic/devices/list.reply s:name s:type i:maxOut i:maxIn f:rate1..rateN`
- Terminator: `← /supersonic/devices/list.done` *(no args)*

### `→ /supersonic/devices/current` *(no args)*

Returns the currently active device.

**Reply:** `← /supersonic/devices/current.reply s:name s:type f:sampleRate i:bufferSize i:activeOutCh i:activeInCh`

### `→ /supersonic/devices/report i:replyPort`

Trigger a full device-state broadcast (the `devices`,
`input-devices`, and `info` trio). `replyPort` is the port the caller
wants those messages sent to; it's registered as a notify target as
a side effect. No direct reply — the three pushes are the answer.

---

## Device control

### `→ /supersonic/devices/switch s:outputName f:sampleRate i:bufferSize [s:inputName]`

Switch the active device. All arguments are positional; the input
name is optional.

**Arguments:**
- `outputName` — output device name, `""` to leave unchanged, or a
  sentinel (`__system__`).
- `sampleRate` — desired rate, `0.0` to keep current / auto-pick.
- `bufferSize` — desired buffer, `0` to keep current / auto-pick.
- `inputName` *(optional)* — input device, `""` to leave unchanged,
  or `__none__` to disable inputs.

**Debouncing:** rapid clicks are coalesced. Each request replaces
the pending switch; the last one executes after a 500 ms quiet
period. The GUI gets an immediate ack — the actual swap happens
asynchronously.

**Reply (immediate):** `← /supersonic/devices/switch.reply i:1`
— always `1`; the request was accepted into the debounce queue.
(The `__system__` and `__none__` sentinel paths reply
synchronously with the real success flag and, on failure, an
error string.)

**Follow-up:** on successful completion, SuperSonic broadcasts
`/supersonic/statechange` and `/supersonic/devices` (via
`sendDeviceReport`) so registered targets see the new state.

### `→ /supersonic/devices/reopen` *(no args)*

Tear down and recreate the current device without changing
selection. Used when an external config change needs to propagate —
e.g. the user bumped the "Computer" channel count in MOTU Pro Audio
Control and wants SuperSonic to re-read the new channel count.

**Debouncing:**
- Rejected if a reopen is already in flight (`"already in progress"`).
- Rejected if less than **3000 ms** have elapsed since the previous
  reopen completed (`"cooldown (<ms> ms since last)"`). The cooldown
  exists because a reopen cold-swaps the World and the Sonic Pi
  Spider layer's `cold_swap_reinit!` takes ~1-3 seconds to reload
  synthdefs and recreate the mixer + groups + scope. A second reopen
  racing an in-flight reinit leaves the client in an inconsistent
  state.

**Reply (immediate):** `← /supersonic/devices/reopen.reply i:accepted s:reason`
- `accepted=1, reason="started"` — queued, worker thread running.
- `accepted=0, reason="already in progress"` or `"cooldown (N ms since last)"`.

**Reply (completion, pushed to notify targets):**
`← /supersonic/devices/reopen.done i:success s:device f:sampleRate i:bufferSize s:error`
- `success=1`: `device`, `sampleRate`, `bufferSize` describe the
  reopened device; `error` is `""`.
- `success=0`: `error` contains a message; the other fields may be
  zeroed.

### `→ /supersonic/devices/mode s:mode`

Set / clear the manual device mode. `mode=""` means "follow system
default output"; anything else pins SuperSonic to that device name
across hot-plug cycles.

**Reply:** `← /supersonic/devices/mode.reply s:currentMode i:success [s:error]`

### `→ /supersonic/inputs/enable i:numChannels`

Enable or disable audio inputs.
- `numChannels == 0` — disable inputs (triggers cold swap).
- `numChannels > 0` — enable that many input channels (triggers
  cold swap if the count differs from the current one).
- `numChannels == -1` — re-enable with the configured boot channel
  count.

**Reply:** `← /supersonic/inputs/enable.reply i:success [i:numChannels | s:error]`

---

## Driver

### `→ /supersonic/drivers/list` *(no args)*

Enumerate available audio drivers (CoreAudio, WASAPI, ALSA, etc.).

**Reply:** `← /supersonic/drivers/list.reply s:currentDriver s:driver1..driverN`

### `→ /supersonic/drivers/switch s:driverName`

Switch audio driver. Forces a cold swap if the new driver's default
device runs at a different rate.

**Reply:**
- `← /supersonic/drivers/switch.reply i:1 s:currentDriver f:sampleRate i:bufferSize` on success.
- `← /supersonic/drivers/switch.reply i:0 s:error` on failure.

---

## Recording

### `→ /supersonic/record/start s:path [s:format] [i:bitDepth]`

Start recording the main output mix to disk.
- `path` — absolute file path.
- `format` — `"wav"` (default), `"flac"`, `"aiff"`, `"ogg"`.
- `bitDepth` — 16 / 24 / 32 (default 24). Ignored for `ogg`.

**Reply:** `← /supersonic/record/start.reply i:success s:pathOrError`

### `→ /supersonic/record/stop` *(no args)*

Stop the active recording and flush the file.

**Reply:** `← /supersonic/record/stop.reply i:success s:pathOrError`

---

## Clock

### `→ /supersonic/clock/offset f:offsetSeconds`

Apply a global NTP-time offset to the engine's scheduler. Used by
Sonic Pi's Spider layer to align SuperSonic's clock with its own
event time. No reply.

---

## Sonic Pi daemon relay

Sonic Pi's GUI can't send to SuperSonic's UDP port directly (the
daemon holds the auth token and brokers on its behalf). For that
architecture, Sonic Pi's `daemon.rb` exposes a parallel `/daemon/audio/*`
surface that simply forwards after token validation:

| Daemon endpoint                 | Forwards to                    |
| ------------------------------- | ------------------------------ |
| `/daemon/audio/switch-device`   | `/supersonic/devices/switch`   |
| `/daemon/audio/switch-driver`   | `/supersonic/drivers/switch`   |
| `/daemon/audio/request-devices` | `/supersonic/devices/list`     |
| `/daemon/audio/reopen-device`   | `/supersonic/devices/reopen`   |

Arguments past the daemon token are passed through unchanged.
SuperSonic's notify-target registration and forwarding logic is
unaware of the daemon — from SuperSonic's perspective, the daemon
is the notify target.

---

## Versioning

There is no explicit protocol version. Breaking changes should
bump the major version in the engine banner (visible in
`/supersonic/info`) and be documented here. Current callers are
Sonic Pi (via `daemon.rb` + `osc_handler.cpp`) and the JS embedder.
