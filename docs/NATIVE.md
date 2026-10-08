# The Native Server

`supersonic` is SuperSonic as a program of its own: scsynth running on
clockwork, with an audio device and a command port. It takes scsynth's
command-line flags and answers scsynth's OSC commands, so a client that starts
and drives scsynth can start and drive it the same way. Sonic Pi does.

This guide is about running it. Building it is in [BUILDING.md](BUILDING.md).
The OSC commands beyond scsynth's own (devices, clock, MIDI and the rest) are
in [OSC_API.md](OSC_API.md).

## Install and first run

After a build, `cmake --install build/native` puts these under the install
prefix:

| Path | What it is |
|---|---|
| `bin/supersonic` | the server |
| `bin/clockwork-plugin-bridge` | the process hosted plugins run in (`clockwork-plugin-bridge.app` on macOS); only in a build with plugin hosting, which is the default |
| `share/man/man1/supersonic.1` | the man page |

```bash
supersonic -v      # prints "SuperSonic <version>" and exits
supersonic -h      # prints the options and exits
supersonic         # opens the default audio device and listens on UDP 57110
```

Once it is up it prints a banner on stderr, under the SuperSonic logo:

```
  SuperSonic v0.89.0

  Compiled: synth Link Link-Audio MIDI Gamepad
  MacBook Pro Speakers (CoreAudio)
  48000 Hz | block 128 | buffer 128 | out 2 | in 0
  UDP
```

`Compiled:` lists what the build has. The next two lines give the device and
its driver, then the rate, the DSP block size, the hardware buffer and the
channels in use; `out 2/8` would mean 2 of the device's 8 outputs. With
`--headless` one line, `headless (no audio device)`, takes their place. The
last line is the command transport — `UDP`, `TCP (max 4 connections)`,
`UDS stream socket (max 4 connections)`, `UDS dgram socket`,
`named pipe (max 4 connections)` or `SHM command plane` — named without its
port or path.

Ctrl-C stops it.

## Command-line options

`-v`, `-h`/`--help` and `--list-devices` are acted on first, wherever they
appear. Every other single-letter flag takes the word after it as its value.

### Audio

| Flag | Default | |
|---|---|---|
| `-H <name>`, `-H <in> <out>` | the system default | The audio device. See [Choosing a device](#choosing-a-device). |
| `-S <rate>` | 48000 | Sample rate. If the device does not offer it, the device's own rate is kept and the log says so. |
| `-Z <frames>` | auto | Hardware buffer size. Auto is the smallest size of 128 or more that the device offers (DirectSound keeps its own). |
| `-z <frames>` | auto | DSP control block size. Auto is the device's buffer when that is 32 to 128, otherwise 128. A value you give is clamped to 32–1024. |
| `-i <n>` | all the device has | Input channels. `0` turns inputs off. |
| `-o <n>` | all the device has | Output channels. |
| `--audio-driver <name>` | the platform's | The driver to boot on. See [Drivers](#drivers). |
| `--list-devices` | | Print the devices and exit. |
| `--headless` | off | No audio device. A timer renders the audio and OSC is still answered. For tests and CI. |

### Network

| Flag | Default | |
|---|---|---|
| `-u <port>` | 57110 | The UDP command port. It also numbers the shared-memory segment. `-u 0` opens neither, so pair it with one of the transports below or the server has no command port. |
| `-B <addr>` | 127.0.0.1 | The address the UDP and TCP command ports bind. The default is IPv4 loopback only: other machines cannot reach the server. `0.0.0.0` binds every IPv4 interface. An empty address (`-B ""`) binds every IPv4 and IPv6 interface for UDP, and every IPv4 interface for TCP. |
| `--tcp <port>` | | TCP command port. |
| `--uds <path>` | | Unix stream socket (macOS, Linux). |
| `--uds-dgram <path>` | | Unix datagram socket (macOS, Linux). |
| `--pipe <name>` | | Named pipe (Windows). |
| `--shm-commands` | | The shared-memory segment's command plane, for one trusted peer on the same machine. Needs `-u` > 0. |
| `--shm-endpoint <path\|pipe>` | from `-u` | Where the shared-memory segment is served. Needs `-u` > 0. |
| `--max-connections <n>` | 4 | Connections allowed at once on TCP, a Unix stream socket or a named pipe, 1–1024. |

Pick at most one of `--tcp`, `--uds`, `--uds-dgram`, `--pipe` and
`--shm-commands`. The one you pick replaces the UDP command port. The cue
server and outbound OSC are unaffected.
[Connecting a client](#connecting-a-client) describes each.

### Engine

These are scsynth's own flags, and go to the engine.

| Flag | Default | |
|---|---|---|
| `-b <n>` | 1024 | Sample buffers, 1–65535 |
| `-n <n>` | 1024 | Nodes (synths and groups) that may exist at once |
| `-d <n>` | 1024 | Synth definitions that may be loaded at once |
| `-w <n>` | 64 | Wire buffers for a synth's internal connections |
| `-a <n>` | 1024 | Audio bus channels |
| `-c <n>` | 16384 | Control bus channels |
| `-m <KB>` | 8192 | Real-time memory pool, in KB |
| `-r <n>` | 64 | Random number generators |
| `-D <0\|1>` | 1 | Load the synthdef directory at boot (see below) |

With `-D 1` the server loads every definition in the synthdef directory before
its command port opens, so a client's first `/s_new` finds them. The directory
is each one in `SC_SYNTHDEF_PATH` (`:` between them, `;` on Windows) when that
is set, else `synthdefs` in the user's application-support directory, as
scsynth reads it. The log says what was loaded. `-D 0` loads nothing.

A value the engine cannot take — out of range, or not a whole number — is
refused with a log line naming it. The process stays up, but no synth runs.
`-m` also sizes the heap the engine takes at boot: the pool plus 16 MB, or the
build's own heap size when that is larger.

### Other

| Flag | Default | |
|---|---|---|
| `--default-bpm <bpm>` | 120 | The tempo the clock opens at. |
| `--app-name <name>` | SuperSonic | The name the operating system shows: PipeWire nodes, ALSA sequencer MIDI clients, macOS aggregate devices, Link peers. |
| `--inbox-mb <MB>` | 512 | The lane loaded samples live in, 1–3072. Address space, not memory: pages are committed as they are written. A load that does not fit fails with "inbox lane full". |
| `-v` | | Print the version and exit. |
| `-h`, `--help` | | Print the options and exit. |

`-U`, `-R`, `-l`, `-I` and `-O` are scsynth's. Each is accepted, with its
value, and ignored.

### Unknown flags and errors

An unknown flag is logged (`unknown flag: --foo`) and skipped, and the server
starts anyway. Check the log when a flag seems to have done nothing.

The server exits with status 1 when:

- two command transports are given;
- `--shm-commands` or `--shm-endpoint` is given with `-u 0`;
- the engine cannot start (the log says why);
- a TCP, Unix socket, named pipe or shared-memory transport cannot start —
  the port is taken, the path cannot be used, the pipe name is held.

A UDP port that cannot be bound is logged and the server carries on.

## Connecting a client

### UDP

The default. The port is open before the audio device is: commands sent while
the device opens (an ASIO device can take more than ten seconds) are held, up
to 1024 packets, and run in order once the engine is ready. Replies go to the
address a command came from. A datagram carries at most 64 KB; use a stream
transport for anything larger.

### TCP

```bash
supersonic --tcp 57120
```

Each OSC packet is preceded by its length, as a 4-byte big-endian integer, in
both directions. A packet may be up to 256 KB. The port binds the `-B`
address. A connection beyond `--max-connections` is closed at once. A
client's notification subscriptions end with its connection.

### Unix sockets (macOS, Linux)

`--uds <path>` is a stream socket framed as TCP is. `--uds-dgram <path>`
carries one OSC packet per datagram; a client that wants replies binds a
socket path of its own. The server removes any file already at the path,
creates the socket and sets its mode to 0600. Put it in a directory only you
can enter (mode 0700), which closes the gap between the two.

### Named pipe (Windows)

`--pipe <name>` serves `\\.\pipe\<name>` (the prefix is added when it is not
given), framed as TCP is. Only the user running the server, and SYSTEM, may
connect. Remote clients are refused. If another process already holds the
name, the server does not start.

### Shared memory

With `-u` > 0 the engine's memory — its rings, audio taps, scope and
metrics — is a shared-memory segment, served to processes of the same user at
an attach endpoint:

| Platform | Endpoint |
|---|---|
| Linux, macOS | `clockwork-shm-<port>.sock` in `$XDG_RUNTIME_DIR`, else in `$TMPDIR`, else `/tmp/clockwork-shm-<uid>-<port>.sock` |
| Windows | `\\.\pipe\clockwork-shm-<port>` |

`--shm-endpoint` serves it elsewhere. A client attaches with clockwork's
client library (`clockwork_client_open_shm` in
`clockwork/src/clockwork_client.h`). With `--shm-commands` the segment's peer
command plane is the command transport. If the segment cannot be served the
server keeps running and says so in the log.

### Files

The engine opens no files. The server answers scsynth's file commands itself,
on a thread of its own, so they work as they do with scsynth. Paths are on
the server's machine. (Over `--shm-commands` they reach the engine directly,
which refuses them.)

| Command | |
|---|---|
| `/b_allocRead`, `/b_allocReadChannel`, `/b_read`, `/b_readChannel` | Read WAV, AIFF, FLAC, Ogg Vorbis, MP3, W64 and RF64. `/b_read` and `/b_readChannel` with `leaveOpen` set are refused: nothing streams from disk. |
| `/b_write` | Writes WAV (`int16`, `int24`, `float`) and FLAC (`int16`, `int24`). Name the header format: scsynth's default, `aiff`, is refused, and so is `leaveOpen`. |
| `/d_load` | A synthdef file, or every `.scsyndef` matching a `*` or `?` pattern in the file name. |
| `/d_loadDir` | Every `.scsyndef` in a directory and the directories below it. |

The replies are scsynth's: `/done` and `/fail` naming the command. A
completion message runs once the load is in.

### Stopping

`/quit`, as with scsynth: the server answers `/done /quit` and shuts down. Or
SIGINT (Ctrl-C) or SIGTERM. Either way the server stops its transports, then the
engine, and exits with status 0.

## Audio devices and drivers

### Listing

```bash
supersonic --list-devices
```

prints each device as `Driver : Device`, with its channels, sample rates and
buffer sizes, marks the open one with `▸`, and exits. AirPlay and Bluetooth
devices are left out. Other flags are ignored. To list the devices it starts
the engine on the default device, then stops it.

### Drivers

| Platform | Default |
|---|---|
| macOS | CoreAudio |
| Windows | Windows Audio (WASAPI, shared mode), or DirectSound if that is missing. Windows Audio (Exclusive Mode), Windows Audio (Low Latency Mode), DirectSound and, in a build with ASIO, ASIO can be asked for. |
| Linux | PipeWire, when the build has it and PipeWire lists devices; otherwise the first of ALSA and JACK that lists devices |

`--audio-driver <name>` boots on another driver. The name is matched exactly,
or without regard to case if only one driver matches. An unknown name is
logged with the drivers there are, and the default is kept. ASIO has no
default device, so it needs `-H` as well. A driver that opens nothing falls
back to the platform's default.

### Choosing a device

`-H` takes the device's name as `--list-devices` prints it:

```bash
supersonic -H "MacBook Pro Speakers"
supersonic -H "MacBook Pro Microphone" "MacBook Pro Speakers"
```

One name names the output and the input. Two name the input, then the output,
as scsynth's `-H` does. The output is a fuzzy match on `Driver : Device`:
every word must appear, case ignored, and the shortest match wins, so a
fragment such as `-H "macbook speakers"` finds the speakers. With
`--audio-driver`, that driver's devices are tried first. On macOS AirPlay and
Bluetooth devices are not candidates. The input is not a fuzzy match: it must
be the device's name. Giving the full name for both is simplest. The log
shows what `-H` matched; when nothing matches it lists the outputs and plays
on the default.

`-H __system__` follows the system default output, as giving no `-H` does.

The device named with `-H` is remembered by that name. If it is missing at
boot, or unplugged later, the server plays on the default and switches back
when a device of that name appears. Choosing another device later, with
`/clockwork/devices/switch`, replaces what is remembered — even choosing the
device that is already playing, as when the named one was missing at boot.

### Following the system default

When no output is named:

- **macOS** follows the system default output when it changes, except to a
  virtual device. If the default is AirPlay or Bluetooth at boot, the server
  opens a non-wireless output instead and stays on it.
- **Windows** follows the default on Windows Audio (WASAPI).
- **Linux** with PipeWire plays on `System Default`, a stream with no fixed
  target that the session manager routes to its default sink.

If the device playing disappears, the server reopens on the default.

### The watchdog

If the device's audio callbacks stop for 2.5 seconds, or run more than 5%
away from the rate it reported for two 5-second windows in a row, the engine
restarts the device. It is always on in the server.

### While it runs

A client lists devices and drivers and switches between them with
`/clockwork/devices/list`, `/clockwork/devices/switch`,
`/clockwork/drivers/switch` and `/clockwork/inputs/enable`, and hears about
changes after `/clockwork/notify`. They are described in
[OSC_API.md](OSC_API.md).

## Clock, Link and scheduling

The clock opens at `--default-bpm` (120). Ableton Link is compiled in (the
banner's `Link` and `Link-Audio`) but off at boot. A client turns it on with
`/clockwork/clock/visibility 1` (peers on this machine only) or `2` (the
network), and off with `0`. The clock verbs, Link tempo and peers, and timed
bundles are in [OSC_API.md](OSC_API.md).

## MIDI, gamepad and OSC cues

- **MIDI.** `/clockwork/midi/ports` lists the ports, and
  `/clockwork/midi/in/enable` and `/clockwork/midi/out/enable` open them.
  Incoming events go to clients that subscribe with
  `/clockwork/midi/notify/subscribe`.
- **Gamepads.** `/clockwork/gamepad/devices` lists them; events go to clients
  that subscribe with `/clockwork/gamepad/notify/subscribe`.
- **OSC cues.** The cue server is off until a client configures it with
  `/clockwork/osc/cue-server/config <port> <loopback> <cues-on>`; loopback
  only is the default. Each message arriving on that port goes, as
  `/external-osc-cue <ip> <port> <address> <args…>`, to clients that
  subscribe with `/clockwork/osc/notify/subscribe`. It is separate from the
  command port.

The details are in [OSC_API.md](OSC_API.md).

## Recording

```
/clockwork/record/start <path> [<format>] [<bits>]
/clockwork/record/stop
```

`<format>` is `wav` (the default) or `flac`. `<bits>` is 16 or 24 (the
default), or 32 for 32-bit float in WAV. The reply is
`/clockwork/record/start.reply 1 <path>`, or `0 <error>`; stopping answers
`/clockwork/record/stop.reply` the same way. The file holds the output as it
goes to the device, from the moment the start is answered, at the device's
rate, with its output channels up to eight. One recording runs at a time.

Recording is the native server's: the server answers these commands and
writes the file, not the engine.

## Plugins and tracks

Only in a build with `CLOCKWORK_PLUGINS=ON`, the default. The Debian package
is built without.

CLAP and VST3 plugins run in a process of their own, the plugin bridge, which
the server starts at boot. If a plugin brings the bridge down the server
starts it again with its tracks, or with none after repeated crashes. The
server looks for the bridge:

1. at the path in `CLOCKWORK_PLUGIN_BRIDGE`;
2. beside its own executable — on macOS as `clockwork-plugin-bridge.app`
   first, then as a plain binary;
3. in the directory the build was configured with
   (`CLOCKWORK_PLUGIN_BRIDGE_DIR`, which a `SUPERSONIC_BRIDGE_INSTALL_DIR`
   other than `bin` sets).

When none is found the log says so.

Plugins are found in the format's own folders, the user's first:

| Platform | Folders |
|---|---|
| macOS | `~/Library/Audio/Plug-Ins/VST3`, `~/Library/Audio/Plug-Ins/CLAP`, `/Library/Audio/Plug-Ins/VST3`, `/Library/Audio/Plug-Ins/CLAP` |
| Windows | `%LOCALAPPDATA%\Programs\Common\VST3` and `\CLAP`, `%COMMONPROGRAMFILES%\VST3` and `\CLAP` |
| Linux | `~/.vst3`, `~/.clap`, `/usr/lib/vst3`, `/usr/local/lib/vst3`, `/usr/lib/clap`, `/usr/local/lib/clap` |

`/clockwork/track/folders/add <dir>` adds a folder, scanned before these.
Tracks, scanning and the rest of `/clockwork/track/` are described in
[clockwork/docs/TRACKS.md](../clockwork/docs/TRACKS.md).

## Logs

The log goes to stderr. There is no log file; redirect stderr to keep one.
The server's lines start `[SuperSonic] ` and the plugin bridge's
`[plugin-bridge] `. `-v`, `--help` and `--list-devices` print to stdout.

`CLOCKWORK_DEV_LOG=1` adds developer lines. Any value counts except an empty
one or one starting with `0`.

A crash prints a line such as
`[SuperSonic] FATAL: SIGSEGV (signal 11) fault addr=0x…`, a backtrace on macOS
and Linux, and `Exiting due to crash.`, and exits with status 128 plus the
signal number: 139 for SIGSEGV.

## Platform notes

### macOS

The server does not ask for microphone access. It usually runs as a
background helper, and macOS denies such a request without showing it, so the
app that starts the server asks, and the permission is that app's — your
terminal, when you start the server by hand. Until access is granted the
server boots with inputs off, checks about once a second, and turns inputs on
when it is granted. If access was denied the log says so, and inputs stay off
until it is granted in System Settings > Privacy & Security > Microphone.

### Windows

The console prints UTF-8, so device names show as they are. The server pumps
Windows messages on its main thread, which is how it hears that devices came
and went. ASIO is only in a build configured with `-DCLOCKWORK_ASIO=ON`, and
needs a device named with `-H`.

### Linux

The drivers are ALSA, JACK and, in a build with the PipeWire headers,
PipeWire. The JACK and PipeWire libraries are loaded at run time, so the
server runs where they are not installed. The PipeWire driver talks to
PipeWire directly, needing neither pipewire-alsa nor pipewire-jack. It offers
`System Default`, each sink and source by name, and `Patchbay (16 ch)`: a node
with 16 ports each way whose first pair is linked to the default sink and
source, the rest left for patching (qpwgraph, pw-link). `--app-name` names
the PipeWire node and the ALSA sequencer client.
