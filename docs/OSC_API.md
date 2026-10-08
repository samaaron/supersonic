# SuperSonic OSC API

The wire reference for the OSC verbs SuperSonic answers beyond scsynth's
own commands. scsynth's commands (`/s_new`, `/d_recv`, `/b_alloc`, …) are
in the [scsynth command reference](SCSYNTH_COMMAND_REFERENCE.md); the
JavaScript embedder API is in [API.md](API.md).

SuperSonic runs scsynth inside clockwork, its audio runtime. Clockwork's
verbs all live under `/clockwork/`. The few messages scsynth itself adds
live under `/supersonic/` ([below](#scsynths-own-messages-supersonic)).

## Notation

| | |
|---|---|
| `→` | a message you send |
| `←` | a reply (to the sender) or a push (to an audience) |
| `i` `h` `f` `d` | int32, int64, float32, float64 |
| `s` `b` `t` | string, blob, OSC timetag |
| `[x]` | optional |
| `x…` | repeated |
| `[tok]` | an optional trailing int32, echoed last in the reply ([correlation tokens](#correlation-tokens)) |

Every verb says which hosts answer it:

| | Host |
|---|---|
| **S** | the native server, `supersonic` |
| **N** | the BEAM NIF ([NIF.md](NIF.md)) |
| **W** | the web client, in a browser |

---

## Transport and conventions

### Getting messages in

**S.** UDP by default.

| Flag | Default | |
|---|---|---|
| `-u <port>` | `57110` | UDP command port. `-u 0` turns UDP off, and the shared-memory segment with it. |
| `-B <addr>` | `127.0.0.1` | Bind address, for UDP and TCP. |

UDP is bound before the engine boots. Up to 1024 packets that arrive
during boot are held and handled in order once it is up.

At most one alternative transport may replace the UDP command port:

| Flag | Transport |
|---|---|
| `--tcp <port>` | TCP, bound to `-B` |
| `--uds <path>` | Unix-domain stream socket (macOS, Linux; file mode 0600) |
| `--uds-dgram <path>` | Unix-domain datagram socket (macOS, Linux; file mode 0600) |
| `--pipe <name>` | Named pipe (Windows; owner-only) |
| `--shm-commands` | The shared-memory segment's command plane: one trusted peer on the same machine. Needs `-u > 0`. |

On TCP, UDS stream and named pipes each OSC packet is preceded by its
length as a 4-byte big-endian integer. A connection is a client: its
subscriptions end when it closes. `--max-connections <n>` caps them
(default 4).

**N.** No socket. `clockwork:send_osc/1` takes one packet. Every reply and
push goes to every registered process as `{osc_reply, Binary}`.

**W.** Through the client: `send(address, ...args)` or `sendOSC(bytes)`.
Replies arrive on its `in` and `in:osc` events.

### Who answers

- An address beginning `/clockwork/` (slash included) is clockwork's.
  Everything else goes to scsynth untouched.
- A `/clockwork/` verb that nothing answers is refused:
  `← /clockwork/error s:address s:reason`. Reasons include
  `unknown clockwork verb` and `malformed`. An unknown `/clockwork/clock/…`
  verb gets [`/clockwork/clock/unsupported`](#clock-and-link) instead.
- **A bundle is scsynth's**, whole and unopened. A `/clockwork/` message
  inside a bundle reaches scsynth and is dropped there. To time a clockwork
  verb, wrap it in [`/clockwork/schedule`](#scheduling).
- A reply goes to the sender: a UDP source address, or a stream
  connection. A push goes to an audience: the notify targets
  ([`/clockwork/notify`](#notify-and-lifecycle)) or a subsystem's
  subscribers.
- `/clockwork/debug s:line` is the engine's log. S prints it to stderr and
  never sends it on the socket; N delivers it as `{debug, Charlist}`; W
  raises the client's `debug` event.
- A reply built off the audio thread is at most 8 KB. The long answers
  (`track/plugins`, `track/plugin/params`) come in pages.

### Correlation tokens

A late reply is otherwise indistinguishable from the reply to the next
request on the same address. Verbs marked `[tok]` take an int32 as their
last argument and echo it as the last argument of the reply; a client that
sends tokens discards a reply whose token does not match. Without a token
the reply is unchanged. Clock verbs whose own last argument is an int32
(`transport/set`, `visibility`, `meter` with two arguments) send no reply,
so the overlap is harmless.

### Subscribe acks

`/clockwork/clock/notify/subscribe`, `/clockwork/midi/notify/subscribe`,
`/clockwork/gamepad/notify/subscribe` and `/clockwork/osc/notify/subscribe`
take an optional trailing int32. With one, the subscribe is acked as
`<verb>.reply i:token`; without, there is no ack. A lost subscribe costs
every event the subsystem would push, so a client that depends on the
stream resends a tokened subscribe until the token comes back. Subscribing
twice is the same as subscribing once.

### Device names

| Name | Where | Meaning |
|---|---|---|
| `__system__` | output | Follow the system default output, on every platform. Not a pin: the engine follows the default as it moves. |
| `System Default` | output | The device table's synthetic row ([`device-table`](#clockworkdevicesreport)). Picking it by name means `__system__`, unless the driver has a real device of that name. |
| `__none__` | input | Inputs off. |

A name with a `" (N)"` duplicate suffix matches with or without it.

---

## Liveness

Answered on the audio thread, in order with everything else sent.

| → | ← | Hosts |
|---|---|---|
| `/clockwork/ping [i:id]` | `/clockwork/pong [i:id]` | S N W |
| `/clockwork/echo s:text` or `b:bytes` | `/clockwork/echo.reply` with the same argument | S N W |
| `/clockwork/sync [i:id]` | `/clockwork/synced i:id` (0 when none was sent), once everything sent before it has reached scsynth | S N W |

---

## Notify and lifecycle

Hosts: **S N**. W refuses these (`unknown clockwork verb`); the web client
raises events instead.

| → | ← |
|---|---|
| `/clockwork/notify` | `/clockwork/notify.reply i:1 s:commit`, then `/clockwork/statechange` with the current state to the caller |
| `/clockwork/notify/unregister` | — |
| `/clockwork/notify/clear` | — (removes every notify target) |

`/clockwork/notify` makes the sender a notify target. `commit` names the
clockwork build. It enumerates no devices: send
[`/clockwork/devices/report`](#clockworkdevicesreport) for the device
list. The replayed state carries the reason `snapshot`, or the error text
when the state is `error`. `/clockwork/setup` is never replayed.

Pushed to notify targets:

| ← | |
|---|---|
| `/clockwork/statechange s:state s:reason` | `state`: `stopped` `booting` `running` `restarting` `error`. `reason`: `init` `boot` `shutdown` `rate-change` `swap-failed-rollback` `swap-no-audio` `swap-recovered` `rebuild-failed` `snapshot`, or scsynth's own text when it failed to build. |
| `/clockwork/setup i:sampleRate i:bufferSize i:generation` | scsynth was rebuilt (a cold swap): everything it held is gone. `generation` counts builds; the first rebuild is 2. |

---

## Devices and drivers

Hosts: **S N**. W refuses every verb here (`unknown clockwork verb`): the
page owns the output.

The verbs that change the device do their work off the control thread. A
device report follows a successful change.

### `/clockwork/devices/report`

`→ /clockwork/devices/report [i:replyPort]`

No argument, or 0: the sender becomes a notify target. `replyPort > 0`:
over UDP, `127.0.0.1:replyPort` becomes one; over any other transport the
sender does, as with no argument. No direct reply: the report is four pushes to
every notify target, in this order (tolerate any):

1. `← /clockwork/device-table s:currentDriver s:intendedDriver i:numDrivers`,
   then per driver `s:driver i:numOutputs (s:name s:flags)… i:numInputs (s:name s:flags)…`.
   One row per driver and device, not deduplicated. `flags` is
   comma-separated: `follows-default`, `exclusive-duplex`, `synthetic`; `""`
   for none. A driver with no default-following device gets a synthetic
   `System Default` output row.
2. `← /clockwork/devices s:mode s:current s:name… i:sampleRate i:compat… s:type…`.
   `mode` is `system` when following the default, otherwise the pinned
   device; `current` is the open output. The names are the strings before
   the first int; then the current rate; then per device `1` if it runs at
   that rate without a rate change; then per device its driver. Wireless
   outputs are hidden; a name shared across drivers appears once, the
   active driver's.
3. `← /clockwork/input-devices s:current i:n s:name… s:type…`
4. `← /clockwork/info s:banner i:sampleRate i:bufferSize i:numRates i:rate… i:numBufs i:buf… i:numDrivers s:driver… s:currentDriver i:outputChannels i:inputChannels i:outputLatencySamples s:intendedDriver`
   - `banner`: several lines of text, for display.
   - Rates: those the output and the open input both offer.
   - Buffer sizes: the powers of two from 16 to 2048 that the device
     offers (everything it offers, when none is), plus the active size when
     it is not among them.
   - `outputChannels`, `inputChannels`: the device's counts; inputs are 0
     when no input is open.
   - `intendedDriver`: a `drivers/switch` pick not yet followed by an open
     device, otherwise `""`. A driver selector shows it when non-empty,
     and `currentDriver` when empty.
   - Fields are only ever appended. Read positionally.

A report is skipped while the enumeration looks mid-change (an open input
missing from the list).

### Queries

| → | ← |
|---|---|
| `/clockwork/devices/list` | per device `/clockwork/devices/list.reply s:name s:type i:maxOutputs i:maxInputs f:rate…`, then `/clockwork/devices/list.done`. Wireless devices are skipped. |
| `/clockwork/devices/current` | `/clockwork/devices/current.reply s:name s:type f:sampleRate i:bufferSize i:activeOutputs i:activeInputs` |
| `/clockwork/drivers/list` | `/clockwork/drivers/list.reply s:currentDriver s:driver…` |

### `/clockwork/devices/switch`

`→ /clockwork/devices/switch s:output f:sampleRate i:bufferSize [s:input]`

- `output`: a device, `""` for no change, `__system__` or `System Default`.
- `sampleRate`, `bufferSize`: `0` keeps the current value or lets the engine
  choose.
- `input`: a device, `""` for no change, or `__none__`.

`← /clockwork/devices/switch.reply i:1` at once, always: the request was
heard. The outcome is one push to the notify targets (none is sent when
there are none):

`← /clockwork/devices/switch.done i:success s:requestedOutput s:requestedInput s:actualOutput s:actualInput s:error i:inputUnavailable s:inputUnavailableReason`

`actualOutput` is where the engine is, whether or not the switch
succeeded: record the user's choice from it. `inputUnavailable` is 1 when
the output opened but the input could not. A device report follows on
success.

- `__system__` / `System Default`: follow the default (as
  `devices/mode ""`).
- Input `__none__`: inputs off. A named output in the same message is not
  switched.
- Otherwise the switch waits 500 ms; a newer switch in that time replaces
  it, and only the one that runs gets a `switch.done`. A switch asked for
  while another runs waits its turn. Naming an output leaves system mode.
- A name over 1024 bytes is refused through `switch.done` (error
  `no audio device has a name that long`, the names cut to 64 bytes).

### Other device verbs

| → | ← |
|---|---|
| `/clockwork/devices/mode s:mode` | `/clockwork/devices/mode.reply s:mode i:ok [s:error]`. `""` follows the system default; a name pins that device across hot-plugs. |
| `/clockwork/devices/reopen` | `/clockwork/devices/reopen.reply i:accepted s:reason`, then `/clockwork/devices/reopen.done i:success s:device f:sampleRate i:bufferSize s:error` to the notify targets |
| `/clockwork/drivers/switch s:driver` | `/clockwork/drivers/switch.reply i:1 s:currentDriver f:sampleRate i:bufferSize`, or `i:0 s:error` |

`devices/reopen` closes and reopens the current device to re-read it (a
channel count changed in the interface's own control panel, say). `reason`
is `started`, `already in progress` or `cooldown (<n> ms since last)`: a
reopen starts at least 3 s after the last one finished.

---

## Inputs

Hosts: **S N**. W refuses it.

`→ /clockwork/inputs/enable i:channels` →
`← /clockwork/inputs/enable.reply i:1 i:channels`, or `i:0 s:error`.

`0` turns inputs off, `n > 0` opens n channels, `-1` restores the count the
engine booted with. The output mode is left alone. A device report follows
on success.

---

## Clock and Link

Addresses in this section are under `/clockwork/clock/`. Times on the wire
are NTP microseconds as `h`, except in `state.reply`.

### Timelines

A verb marked † may name a timeline: `/clockwork/clock/<timeline>/<verb>`.

| Timeline | |
|---|---|
| `link` | The session clock (Ableton Link when enabled). The default when none is named. |
| `midi` | The primary MIDI clock follower. |
| `midi:<port>` | The follower for one MIDI input's clock. A write (`tempo/set`, `transport/set`, `meter`, `midi/clock/follow`) claims one that has not clocked yet. |

The reply carries the same segment, e.g. `/clockwork/clock/midi:foo/tempo.reply`.

### Core verbs

Hosts: **S N W**.

| → | ← | |
|---|---|---|
| `tempo/set f:bpm [h:atNtpMicros]` † | — | From now, or from the instant given (the beat then is held). On a `midi…` timeline, an override until its clock next ticks. |
| `tempo/get [tok]` † | `tempo.reply d:bpm [tok]` | |
| `transport/set i:playing` † | — | On a `midi…` timeline, play is a Start at beat 0. |
| `transport/get [tok]` † | `transport.reply i:playing i:anchored [tok]` | `anchored`: a Start or Song Position has fixed the beat origin (always 1 for `link`). |
| `transport/time/get [tok]` † | `transport/time.reply h:ntpMicros [tok]` | The last transport change; 0 if none. |
| `meter i:num i:den` † | — | |
| `meter [tok]` † | `meter.reply i:num i:den [tok]` | |
| `bar [tok]` † | `bar.reply d:bar d:beatInBar i:num i:den [tok]` | Now. Bar 0 starts at beat 0. |
| `rpc/beat_at_time h:ntpMicros f:quantum [tok]` † | `rpc/beat_at_time.reply d:beat [tok]` | |
| `rpc/phase_at_time h:ntpMicros f:quantum [tok]` † | `rpc/phase_at_time.reply d:phase [tok]` | In `[0, quantum)`. |
| `rpc/time_at_beat h:microbeats f:quantum [tok]` † | `rpc/time_at_beat.reply h:ntpMicros [tok]` | 1 beat = 1 000 000 microbeats. `f:beat` is accepted but loses precision past beat 32768. |
| `rpc/beat_phase_at_time h:ntpMicros f:quantum [tok]` † | `rpc/beat_phase_at_time.reply d:beat d:phase [tok]` | |
| `rpc/beat_phase_now f:quantum [tok]` † | `rpc/beat_phase_now.reply h:ntpMicros d:beat d:phase [tok]` | |
| `timelines/get [tok]` | `timelines.reply (s:name s:label f:bpm i:clocking i:stale i:primary)… [tok]` | `label` is the OS device name. |
| `start_stop_sync/set i:on` | — | Link start/stop sync. |
| `start_stop_sync/get [tok]` | `start_stop_sync.reply i:on [tok]` | |
| `enabled/get [tok]` | `enabled.reply i:linkEnabled [tok]` | |
| `time/now/get [tok]` | `time/now.reply h:ntpMicros [tok]` | |
| `peers/count/get [tok]` | `peers/count.reply i:n [tok]` | |
| `capabilities/get [tok]` | `capabilities.reply (s:name i:value)… [tok]` | `link`, `link_audio`, `midi`. Match by name. All 0 on W. |

`← /clockwork/clock/unsupported s:address [tok]` answers a clock verb this
host does not have (the Link verbs on W) or that does not exist.

### `state/get`

`→ /clockwork/clock/state/get [tok]` →
`← /clockwork/clock/state.reply d:bpm i:playing d:beatOriginNtp d:playingChangedAtNtp i:flags i:meterNum i:meterDen [tok]`

The whole clock from one snapshot, answered on the audio thread. Times are
NTP seconds. `flags`: bit 0 Link enabled, bit 1 start/stop sync, bit 2
Link Audio publishing.

Hosts: **S N W**.

### Link session

Hosts: **S N** (capability `link`).

| → | ← | |
|---|---|---|
| `visibility i:mode` | — | 0 off, 1 this machine only, 2 the network. |
| `visibility/get [tok]` | `visibility.reply i:mode [tok]` | |
| `peer_name/set s:name` | — | The name other peers see. |
| `peer_name/get [tok]` | `peer_name.reply s:name [tok]` | |
| `peers/get` | `peers.reply i:n (s:nodeId s:gatewayIp i:isLoopback s:measurementIp i:measurementPort s:audioIp i:audioPort)…` | `audioIp` is `""` for a peer without Link Audio. |
| `reset` | — | Leave the session and rejoin it. |
| `notify/subscribe [i:token]` | `notify/subscribe.reply i:token` if tokened, then `notify/tempo` and `notify/peers` to the caller | |
| `notify/unsubscribe` | — | |

Pushed to clock subscribers:

| ← | |
|---|---|
| `/clockwork/clock/notify/tempo d:bpm` | |
| `/clockwork/clock/notify/peers i:n` | |
| `/clockwork/clock/notify/transport i:playing h:atNtpMicros` | |
| `/clockwork/clock/timelines (s:name s:label f:bpm i:clocking i:stale i:primary)…` | The set of timelines changed (builds with MIDI). |

### Link Audio

Hosts: **S N** (capability `link_audio`).

| → | ← | |
|---|---|---|
| `audio/publish/set i:on` | — | Publish this engine's audio to peers. |
| `audio/publish/get [tok]` | `audio/publish.reply i:on [tok]` | |
| `audio/channels/get` | `audio/channels.reply i:n (s:channelId s:channelName s:peerId s:peerName)…` | Channels the peers offer. |
| `audio/input/add s:peer s:channel i:inputChannel` | `audio/input/add.reply i:ok` | The peer's channel onto input channels `inputChannel` and `+1` (mono is mirrored). Refused unless the channel exists and the pair is free. |
| `audio/input/remove s:peer s:channel` | — | |
| `audio/input/clear` | — | |
| `audio/input/latency/set s:peer s:channel f:seconds` | `audio/input/latency/set.reply i:ok` | |
| `audio/inputs/get` | `audio/inputs.reply i:n (s:peer s:channel i:inputChannel i:sampleRate i:sourceChannels f:bufferedMs i:state i:droppedSourceBuffers i:networkGapBuffers i:totalSourceBufferCalls i:duplicateCountCalls f:latencySeconds)…` | `state`: 0 not subscribed, 1 connecting, 2 connected, 3 dropout. `sourceChannels` is 0 until the first buffer. |
| `audio/sink/add s:name i:channel i:numChannels` | `audio/sink/add.reply i:ok` | An extra published channel, from `numChannels` channels starting at `channel`. |
| `audio/sink/remove s:name` | — | |
| `audio/sinks/get` | `audio/sinks.reply i:n (s:name i:channel i:numChannels i:hasSubscriber)…` | |

---

## Scheduling

### `/clockwork/schedule`

`→ /clockwork/schedule h:when b:message`

Hosts: **S N W**.

Holds `message` (one OSC message, clockwork's or scsynth's) and handles it
at `when` as if it had arrived then from the same sender, so its reply
comes back to you. `when` is an OSC timetag, as `t` or `h`, or NTP
seconds as `d` or `f`; 0 or 1 means now. No reply. A malformed one is
refused (`malformed`) and counted in the metrics.

A scheduled MIDI or OSC send leaves at its moment: the time goes with it
to the port or the socket.

A bundle with a future timetag is also held, and handed to scsynth whole
at its time.

### `/clockwork/sched/flush`

`→ /clockwork/sched/flush [s:tag]`

Hosts: **S N W**.

Drops what is pending under `tag`. No reply; a `tag` that is not a string
is refused (`malformed`). No tag, or `""`, is `default`:
everything held by `/clockwork/schedule`. `synth` is the timestamped
bundles held for scsynth.

---

## MIDI

Hosts: **S N**, and **W** when the page opts in with `midi: true` in the
client's options. Without it, W refuses every `/clockwork/midi/` verb with
`MIDI is not enabled on this host: new Clockwork({ midi: true })`.

- A port is a normalised handle: lowercase, with space `# * , / ? [ ] { } :`
  replaced by `_`, and duplicates suffixed `_2`, `_3`, …
- `"*"` is every open port.
- Channels are 1 to 16; `-1` on output is all 16.

### Ports

Addresses under `/clockwork/midi/`.

| → | ← | Hosts |
|---|---|---|
| `ports/list` (or `ports/get`) | `ports.reply i:nIn (s:port i:open)… i:nOut (s:port i:open)…` | S N W |
| `in/enable s:port i:on` | — (pushes `ports`) | S N W |
| `out/enable s:port i:on` | — (pushes `ports`) | S N W |
| `refresh` | — (re-enumerates, pushes `ports`) | S N W |
| `notify/subscribe [i:token]` | a `ports.reply` snapshot, then `notify/subscribe.reply i:token` if tokened | S N W |
| `notify/unsubscribe` | — | S N W |

### Sending

`→ /clockwork/midi/out/<verb> s:port <args> [t:when]`. No reply. A
trailing timetag sends at that moment. A send whose arguments cannot be
read is refused (`malformed`).

| Verb | Arguments after `port` |
|---|---|
| `note_on`, `note_off` | `i:channel i:note i:velocity` |
| `control_change` | `i:channel i:controller i:value` |
| `program_change` | `i:channel i:program` |
| `channel_pressure` | `i:channel i:value` |
| `poly_pressure` | `i:channel i:note i:value` |
| `pitch_bend` | `i:channel i:value` (0–16383) |
| `raw`, `sysex` | `i:byte…`, or one `b:bytes` |
| `clock`, `start`, `stop`, `continue` | none |

Hosts: **S N W**.

### Clock out

| → | ← | Hosts |
|---|---|---|
| `clock/tick s:port` | — | S N W |
| `clock/beat s:port f:durationMs` | — | S N |
| `clock/follow s:port [s:timeline] [tok]` | `clock/follow.reply s:port s:timeline [tok]` | S N |
| `clock/unfollow s:port [tok]` | `clock/unfollow.reply s:port [tok]` | S N |
| `clock/followers [tok]` | `clock/followers.reply (s:port s:timeline)… [tok]` | S N |

- `clock/tick`: one clock byte (0xF8) now.
- `clock/beat`: one beat of 24 ticks, spread over `durationMs` from now. No
  transport byte: send `out/start` / `out/stop` / `out/continue` yourself.
- `clock/follow`: a continuous 24-per-beat clock on the timeline's grid
  (`link` by default, `midi` or `midi:<port>`), with Start and Stop sent at
  its transport changes. A port already following is re-targeted. At most 8
  ports. An unknown timeline or a ninth port is refused in the log, with no
  reply.
- W refuses `clock/beat`, `clock/follow`, `clock/unfollow` and
  `clock/followers` with `not available on the web host`.

### Clock in

`→ /clockwork/midi/clock/sync s:port i:on`: heed (1, the default) or ignore
(0) the MIDI clock arriving on an input. Hosts: **S N W**.

On S and N an input's clock drives its `midi:<port>` timeline: the pulses
set its tempo, and Start, Continue, Stop and Song Position its transport.
On W the estimated tempo arrives as `/clockwork/midi/in/clock_bpm`.

### Events

Pushed to MIDI subscribers as `← /clockwork/midi/in/<kind> s:port <args>`:

| Kind | Arguments after `port` |
|---|---|
| `note_on`, `note_off` | `i:channel i:note i:velocity` |
| `control_change` | `i:channel i:controller i:value` |
| `program_change` | `i:channel i:program` |
| `channel_pressure` | `i:channel i:value` |
| `poly_pressure` | `i:channel i:note i:value` |
| `pitch_bend` | `i:channel i:value` |
| `sysex` | `b:bytes` |
| `song_position` | `i:sixteenths` |
| `song_select` | `i:song` |
| `time_code` | `i:data` |
| `tune_request`, `start`, `continue`, `stop`, `active_sensing`, `reset` | none |
| `clock_bpm` | `f:bpm` (W only) |

Clock pulses (0xF8) are not forwarded. On W each event but `clock_bpm`
carries a trailing `t`: when it arrived.

`← /clockwork/midi/ports` (the `ports.reply` payload) is pushed when a port
appears, goes or is enabled.

---

## Gamepad

Hosts: **S N**, and **W** when the page opts in with `gamepad: true`.
Without it, W refuses every `/clockwork/gamepad/` verb with
`gamepad is not enabled on this host: new Clockwork({ gamepad: true })`.

A pad is a normalised handle, as a MIDI port is; `"*"` is every connected
pad. Hot-plug is automatic. A pad is enabled when it connects, unless
`enable "*" 0` said otherwise.

On macOS, discovery needs the host process to pump the main run loop. The
server does; the NIF does not, so on macOS it answers every verb but lists
no pads.

| → | ← | Hosts |
|---|---|---|
| `/clockwork/gamepad/devices/list` (or `devices/get`) | `/clockwork/gamepad/devices.reply i:n (s:pad i:enabled)…` | S N W |
| `/clockwork/gamepad/enable s:pad i:on` | — (pushes `devices`). `"*"` also sets the default for pads that connect later. | S N W |
| `/clockwork/gamepad/refresh` | — (pushes `devices`) | S N W |
| `/clockwork/gamepad/notify/subscribe [i:token]` | a `devices.reply` snapshot, then `notify/subscribe.reply i:token` if tokened | S N W |
| `/clockwork/gamepad/notify/unsubscribe` | — | S N W |
| `/clockwork/gamepad/out/rumble s:pad f:strong f:weak i:durationMs` | — | S N W |
| `/clockwork/gamepad/out/rumble_stop s:pad` | — | S N W |

Rumble magnitudes are 0 to 1; `durationMs <= 0` rumbles until stopped. It
is ignored on macOS and by pads without force feedback; on W it uses
`Gamepad.vibrationActuator`.

Pushed to gamepad subscribers:

| ← | |
|---|---|
| `/clockwork/gamepad/in/button s:pad s:button i:pressed f:value` | `value` 0 to 1; triggers sweep, other buttons jump. |
| `/clockwork/gamepad/in/axis s:pad s:axis f:value` | `value` −1 to 1, up and right positive. |
| `/clockwork/gamepad/devices i:n (s:pad i:enabled)…` | A pad connected, went or was enabled. |

Buttons: `south east west north left_shoulder right_shoulder left_trigger
right_trigger select start left_thumb right_thumb dpad_up dpad_down
dpad_left dpad_right mode`. Axes: `left_x left_y right_x right_y`, and
`dpad_x dpad_y` for a pad whose d-pad is a hat. Anything beyond is
`button_<i>` or `axis_<i>`. Values move in steps of 1/127 and only changes
are sent (with a 0.08 stick dead zone). On W each event carries a trailing
`t`: when it was seen.

---

## OSC cues and sending

Hosts: **S N**. W refuses these.

| → | ← |
|---|---|
| `/clockwork/osc/cue-server/config i:port i:loopback i:cuesOn` | — |
| `/clockwork/osc/cue-server/cues-on i:on` | — |
| `/clockwork/osc/cue-server/loopback i:on` | — |
| `/clockwork/osc/notify/subscribe [i:token]` | `/clockwork/osc/notify/subscribe.reply i:token` if tokened |
| `/clockwork/osc/notify/unsubscribe` | — |

The cue server listens on `port` (0: not at all, the default), on
127.0.0.1 and ::1 when `loopback` is 1 (the default), on every interface
when 0. The flags also take `T`/`F`. With cues on (off by default), every
message it receives is pushed to OSC subscribers as

`← /external-osc-cue s:ip i:port s:address <the message's arguments>`

outside the `/clockwork/` namespace; a bundle arrives as its messages.

On N the cues arrive as `{osc_reply, Binary}` at every registered process.

### `/clockwork/osc/send`

`→ /clockwork/osc/send s:host i:port b:packet`

Sends `packet` to `host:port` (IPv4 or IPv6; a hostname is resolved once).
Inside `/clockwork/schedule` it leaves at its time. No reply on success.
A send that fails is refused with `host does not resolve`,
`message of <n> bytes exceeds the sink's widest cell (<m> bytes)` or
`sink full`. An empty host, a port of 0 or less, or an empty packet is
refused (`malformed`).

---

## Recording

Hosts: **S** and **N**, which both send through `supersonic::Commands`. W
refuses these (`unknown clockwork verb`), and so does S over `--shm-commands`.

| → | ← |
|---|---|
| `/clockwork/record/start s:path [s:format] [i:bits]` | `/clockwork/record/start.reply i:ok s:pathOrError` |
| `/clockwork/record/stop` | `/clockwork/record/stop.reply i:ok s:pathOrError` |

Records the main output from the moment `start` is answered, at the
device's rate and channel count. `path` is UTF-8 on every platform,
Windows included.

| `format` | `bits` |
|---|---|
| `wav` (default; also `wave`) | 16, 24 (default), 32 (float) |
| `flac` | 16, 24 |

`aiff`, `w64` and `rf64` are recognised but refused, as is any other pair:
`unsupported format/bitDepth: <format>/<bits>`. Other errors:
`already recording`, `not recording`, `could not open '<path>' for writing: <status>`,
`'<path>' could not be finished: <status>`.

---

## Tracks and plugins

Hosts: **S**. W refuses these. N accepts them, but its tracks do not play
yet.

A track is a named chain of CLAP and VST3 plugins with a stereo send and
return lane; the plugins run in a separate bridge process.
[clockwork/docs/TRACKS.md](../clockwork/docs/TRACKS.md) has the model.
Addresses are under `/clockwork/track/`. `<track>` is `i:id` or `s:name`.
An unknown track verb is refused with `unknown track verb`.

### Playing

Handled inside the audio block, so `/clockwork/schedule` places them on
their sample. No replies.

| → | |
|---|---|
| `note <track> i:on i:pitch f:velocity [i:channel]` | `velocity` 0 to 1, or `i` 0 to 127. |
| `cc <track> i:number f:value [i:channel]` | `value` 0 to 1, or `i` 0 to 127. |
| `bend <track> f:bend [i:channel]` | −1 to 1. |
| `notes_off [<track>]` | No track, or `"*"`: every track. |
| `param <track> s:name f:value [i:handle]` | By parameter name: the first plugin in the chain that has it, or the one `handle` names. |
| `plugin/param i:handle i:id f:value` | By parameter id, in the plugin's own range. |

`channel` 1 to 16 reaches the instruments listening on that channel
(`plugin/channel`); 0 or none reaches all. Channel 16 is read as 0.

### Editing

| → | ← |
|---|---|
| `list` | `list.reply` (the `track/list` payload), and a `track/list` broadcast |
| `create s:name` | `create.reply i:id i:ok s:nameOrError` |
| `remove <track>` | `remove.reply i:id i:removed` |
| `rename <track> s:name` | `rename.reply i:id i:ok s:nameOrError` |
| `move <track> i:index` | — |
| `clear` | — |
| `gain <track> f:gain` | — (linear, 1 is unity) |
| `mute <track> i:mute` | — |
| `timeline <track> [s:timeline]` | `timeline.reply i:id i:ok s:timelineOrError`. `link` (the default) or `midi:<port>`; no name asks. |
| `plugin/add <track> s:path [i:index] [i:at]` | `plugin/add.reply i:handle i:ok s:nameOrError s:path`. `index`: which plugin in the file; `at`: chain position (the end by default). |
| `plugin/remove i:handle` | `plugin/remove.reply i:handle i:removed` |
| `plugin/move i:handle i:index` | — |
| `plugin/bypass i:handle i:bypass` | — |
| `plugin/channel i:handle i:channel` | — (1 to 16; 0 is every channel) |
| `plugin/editor i:handle i:show` | — (the plugin's own window) |
| `plugin/params i:handle [i:offset]` | one `track/plugin/params` page, broadcast |
| `rig/save s:path` | `rig/save.reply i:ok s:pathOrError` |
| `rig/load s:path` | `rig/load.reply i:ok i:missing s:pathOrError`. Replaces every track. |
| `folders` | `folders.reply i:nExtra s:dir… i:nPlatform s:dir…`, and a broadcast |
| `folders/add s:dir`, `folders/remove s:dir` | — |
| `scan` | `plugins.reply i:total i:offset i:count (s:name s:vendor s:format s:path i:index i:isInstrument)…` in pages of up to 64, each also broadcast as `track/plugins`. Takes seconds. |

### Broadcasts

To the notify targets, after every change:

| ← | |
|---|---|
| `/clockwork/track/list i:laneBase i:count …` | Per track `i:id i:slot s:name i:sendChannel i:returnChannel f:gain i:mute i:nodeCount`, then per plugin `i:handle i:isInstrument i:bypass i:channel s:name s:vendor s:format s:path i:index i:latency`; after the last track, per track `s:timeline`. Channels are 0-based. |
| `/clockwork/track/state i:id f:gain i:mute s:timeline` | Gain or mute changed. |
| `/clockwork/track/folders …` | As `folders.reply`. |
| `/clockwork/track/plugins …` | As `plugins.reply`. |
| `/clockwork/track/plugin/params i:handle i:total i:offset i:count (i:id s:name f:min f:max f:value i:group s:groupName i:automatable)…` | Up to 48 per page: ask for `offset + count` next. Every subscriber hears every page. |
| `/clockwork/track/plugin/param/edit i:handle i:id f:normalised` | The plugin's own window moved a control. |
| `/clockwork/track/plugin/param/value i:handle i:id f:normalised` | A parameter set by name. |
| `/clockwork/track/error s:verb s:detail i:handle` | A verb failed; `handle` is the plugin concerned, or 0. `verb` `bridge`: the plugin process died, hung or could not start, and is restarting. |

---

## Assets

Hosts: **S W**. N answers, but gives a client no way to write the inbox.

`→ /clockwork/asset/commit i:id i:kind i:offset i:bytes i:channels i:frames f:rate`

Hands scsynth bytes the client wrote into the inbox lane at
`offset`…`offset + bytes`. SuperSonic's scsynth takes `id` as the buffer
number. On S the inbox is in the shared-memory segment, sized by
`--inbox-mb` (default 512); on W it is in the module's memory.

| `kind` | |
|---|---|
| 0 | Raw bytes. `channels`, `frames` and `rate` are ignored. |
| 1 | Interleaved float32 audio. `frames × channels × 4` must equal `bytes`; `rate > 0`. |

| ← | |
|---|---|
| `/clockwork/asset/committed i:id` | scsynth has it. |
| `/clockwork/asset/refused i:id s:reason` | e.g. `range runs past the inbox`, `id is still held; release it first`, `the guest refused it`. |
| `/clockwork/asset/released i:id` | Later, to the committing client: scsynth let it go (a `/b_free`, or a rebuild). The range may be reused. |

---

## Misc

### `/clockwork/summary`

Hosts: **S**. N and W refuse it.

No reply. Writes two lines to the server's log: its name and version, and
what was compiled in. For a GUI that shows the log.

### Versioning

There is no protocol version. `/clockwork/notify.reply` names the
clockwork build, `supersonic -v` prints the server's version, and
`/clockwork/clock/capabilities/get` says which clock features a build has.
Replies grow only by appending arguments: read positionally, and ignore
what follows the fields you know.

---

## scsynth's own messages (`/supersonic/`)

scsynth answers these itself, on every host.

| | |
|---|---|
| `← /supersonic/buffer/allocated s:uuid i:bufnum` | After a `/b_allocPtr`. |
| `← /supersonic/buffer/freed i:bufnum h:pointer` | A buffer's memory was freed. |
| `← /supersonic/synthdef/loaded s:name` | Per definition after a `/d_recv`. W only. |
| `→ /supersonic/buffer/read i:bufnum i:bufOffset i:inboxOffset i:frames i:channels f:rate` | Copies interleaved frames from the inbox into an allocated buffer. `/done` or `/fail`. |
| `→ /supersonic/buffer/publish i:bufnum i:bufOffset i:frames i:outboxOffset` | Copies a buffer's frames into the outbox (`frames < 0`: to the end). `← /supersonic/buffer/published i:bufnum i:channels i:frames f:rate`, or `/fail`. |
| `→ /supersonic/piano/wavetable [i:bufnum]` | The piano UGen plays from channel 0 of `bufnum`; none, or -1, takes the table away. `/done` or `/fail`. |
