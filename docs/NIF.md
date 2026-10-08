# NIF (Erlang/Elixir)

> **Experimental.** Nothing has been built on the NIF yet beyond its tests, and
> it has not driven a real audio device in anger. Expect rough edges, and an
> API that changes as something real is built on it.

SuperSonic also builds as a NIF: a shared library the BEAM loads, with
scsynth running inside the VM. OSC goes in as binaries and comes back as
Erlang messages. There is no command port and no separate process.

The NIF is clockwork's (`clockwork/src/nif`): `clockwork_nif.cpp` is the
library, and `clockwork.erl` is the Erlang module that loads it. Built here,
it sends every packet through `supersonic::Commands`, as SuperSonic's server
does, so a BEAM client is answered as a socket client is.

## Build

```bash
git clone --recurse-submodules https://github.com/samaaron/supersonic
cd supersonic
cmake -B build/nif -DCLOCKWORK_NIF=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build/nif --target clockwork_nif --config Release --parallel
```

The prerequisites are the native server's: see
[Building from Source](BUILDING.md#native-server). clockwork vendors the
`erl_nif` headers, so the build needs no Erlang installation.

The library lands in the build root, whatever the configuration:

| Platform | Library |
|---|---|
| macOS, Linux | `build/nif/clockwork.so` |
| Windows | `build/nif/clockwork.dll` |

It needs Erlang/OTP 26 or later: the vendored headers are NIF API 2.17,
which OTP 26 introduced.

## Loading

The Erlang module is `clockwork` (`:clockwork` from Elixir). Compile
`clockwork/src/nif/clockwork.erl` into your project, as `test/nif/mix.exs`
does with `erlc_paths: ["../../clockwork/src/nif"]`, or by hand with `erlc`.

The module loads the library as it is itself loaded (`-on_load`), from the
first of these that applies:

1. the directory named by `CLOCKWORK_NIF_PATH`;
2. the `priv` directory of an application called `clockwork`;
3. the directory holding `clockwork.beam`.

`CLOCKWORK_NIF_PATH` names the directory, not the file. The module loads
`<dir>/clockwork` and the BEAM adds `.so` or `.dll`.

If the library cannot be loaded, the module does not load either. A warning
gives the reason, and any call to `:clockwork` raises `UndefinedFunctionError`
(`undef` in Erlang).

Hot upgrade is not supported. Loading a new version of the module while the
NIF is loaded fails with `{upgrade, "Upgrade not supported by this NIF
library."}` and the old code stays; restart the VM to take a new build.
`start/1` and `stop/0` may be called any number of times in one VM.

## API

| Function | Returns |
|---|---|
| `is_nif_loaded()` | `true` |
| `start(Config)` | `ok`, then a `{clockwork_started, _}` message; `badarg` for anything but a map |
| `stop()` | `ok`, then a `{clockwork_stopped, ok}` message |
| `send_osc(Binary)` | `ok`, `{error, full}`, `{error, too_big}` or `{error, not_running}`; `badarg` for anything but a non-empty binary |
| `set_notification_pid()` | `ok`; the calling process now receives the engine's messages |
| `clear_notification_pid()` | `ok`; the calling process no longer does, the others still do |

`start/1` and `stop/0` return at once. The NIF's own thread then boots or
stops the engine, and sends the outcome to the process that called:

- `{clockwork_started, ok}`
- `{clockwork_started, {error, already_running}}`: an engine is already running
- `{clockwork_started, {error, Reason}}`: `Reason` is a charlist saying why
  nothing is running: an option the NIF cannot read, one scsynth refuses (it
  names the line), or a heap the system cannot provide
- `{clockwork_stopped, ok}`: also sent when nothing was running

`send_osc/1` takes one OSC message or bundle per call. `{error, full}` means
the engine's input ring had no room at that moment: the engine drains it every
block, so try again. `{error, too_big}` means the packet is larger than the ring
can ever hold: bulk data such as samples belongs in a file the engine is told
about (`/b_allocRead`), not in a packet.

## Start options

`start/1` takes a map. Five atom keys are the device's:

| Key | Default | |
|---|---|---|
| `headless` | `false` | `true` runs without an audio device |
| `sample_rate` | `48000` | the rate asked of the device |
| `buffer_size` | `0` | the device's buffer, in samples; 0 lets clockwork choose |
| `num_output_channels` | all the device has | 2 when headless |
| `num_input_channels` | all the device has | 0 when headless |

`headless` takes `true` or `false`; the others take integers. A key that is
not an atom, or a value of the wrong kind, is refused with `{error, Reason}`
naming the key; nothing is booted.

Every other atom key goes to scsynth as a `name=value` line. A value is an
integer, a float, `true` or `false` (sent as 1 and 0), or a binary; anything else
is refused. scsynth's options are all non-negative integers, and it refuses a
name it does not know, or a value it will not take, with a reason naming the
line. scsynth matches a name ignoring case,
`_` and `-`, so `max_nodes` is its `maxNodes`. Its options, from
`dsp/scsynth/scsynth_options.h`:

| Key | Default | |
|---|---|---|
| `num_buffers` | 1024 | sample buffers |
| `max_nodes` | 1024 | nodes (synths and groups) that may exist at once |
| `max_graph_defs` | 1024 | synth definitions that may be loaded at once |
| `max_wire_bufs` | 64 | wire buffers for a synth's internal connections |
| `num_audio_bus_channels` | 1024 | audio bus channels |
| `num_control_bus_channels` | 16384 | control bus channels |
| `real_time_memory_size` | 8192 | the real-time memory pool, in KB |
| `num_rgens` | 64 | random number generators |
| `verbosity` | 0 | how much the engine prints; 0 is quiet |

Load synth definitions with `/d_recv` once the engine is running (see below).

The lines go to scsynth in a 1024-byte block with a terminating NUL. A map that does not fit fails the boot with
`{clockwork_started, {error, Reason}}`.

The NIF opens no command port and creates no shared-memory segment: OSC
reaches the engine only through `send_osc/1`.

## Messages to registered processes

A process that has called `set_notification_pid/0` receives:

- `{osc_reply, Binary}`: an OSC packet from the engine, a reply or a broadcast
- `{debug, Charlist}`: a line of the engine's log

Any number of processes may register, and every one receives every message,
including the replies to OSC that another process sent: a reply does not go
to the process that asked, but to all of them. Registering twice is the same
as registering once. A registered process that has exited is dropped at the
next delivery.

`stop/0` clears every registration, so register again after each `start/1`.
A process that registers before `start/1` also receives the boot's log lines.

## Controlling the engine

The engine takes scsynth's OSC commands: see the
[scsynth command reference](SCSYNTH_COMMAND_REFERENCE.md). A few to start
with:

- `/d_load` with a `.scsyndef` path, or `/d_loadDir` with a directory, loads
  synth definitions and answers `/done`. `/d_recv`, with the bytes of a
  `.scsyndef` file as a blob, does the same without a file.
- `/b_allocRead` and the other buffer file commands, and
  `/clockwork/record/start` and `/stop`, work as they do on the server
  ([native guide](NATIVE.md#files)). Paths are on the machine the VM runs on,
  and the files are read off the audio thread.
- `/quit` is refused with `/fail`: stop the engine with `stop/0`.
- A command scsynth does not know is answered with
  `/fail <command> "Command not found"`.
- `/notify 1` sends node events (`/n_go`, `/n_end`, ...) to the registered
  processes.
- `/status` and `/version` answer `/status.reply` and `/version.reply`.

clockwork keeps the `/clockwork/` prefix for its own verbs. Among them:

| Send | Back |
|---|---|
| `/clockwork/notify` | `/clockwork/notify.reply` (1, clockwork's commit), then `/clockwork/statechange` (state, reason). From then on the engine also sends its lifecycle and device broadcasts, such as `/clockwork/statechange` and `/clockwork/setup`. |
| `/clockwork/clock/notify/subscribe` | `/clockwork/clock/notify/tempo` (a double, in BPM) and `/clockwork/clock/notify/peers` (an int32) now, then as they change |
| `/clockwork/clock/tempo/get` | `/clockwork/clock/tempo.reply` (a double, in BPM) |

The state is `booting`, `running`, `restarting`, `stopped` or `error`; with
`error`, the reason says why. An address under `/clockwork/` that nothing
handles is answered with `/clockwork/error` (the address, then the reason).

## Threads

- One engine per VM. A `start/1` while one is running reports
  `{error, already_running}`.
- `start/1` and `stop/0` queue their work for one thread, started when the
  library loads, so they take effect in the order they were called and never
  block a scheduler. No NIF uses a dirty scheduler.
- `send_osc/1` may be called from any process. It is a write to the engine's
  input ring, and does not wait for the engine to act on it.
- When the BEAM purges the module, the library's `on_unload` stops that thread
  and shuts down a running engine. A normal halt does not purge it; the
  operating system reclaims the engine as the VM exits.

## Example

Boot the engine without an audio device, ask scsynth for its version, and
stop. Save this as `hello.exs` in the repository root:

```elixir
# Boot the engine without an audio device, ask scsynth for its version, stop.
:ok = :clockwork.start(%{headless: true})

receive do
  {:clockwork_started, :ok} -> :ok
  {:clockwork_started, {:error, reason}} -> raise "boot failed: #{inspect(reason)}"
end

:ok = :clockwork.set_notification_pid()

# An OSC string is NUL-terminated and padded with NULs to a multiple of 4 bytes.
osc_string = fn s ->
  s = s <> <<0>>
  s <> :binary.copy(<<0>>, rem(4 - rem(byte_size(s), 4), 4))
end

# /version with no arguments: the address, then an empty type tag string.
:ok = :clockwork.send_osc(osc_string.("/version") <> osc_string.(","))

receive do
  {:osc_reply, "/version.reply" <> _ = reply} -> IO.inspect(reply, binaries: :as_strings)
after
  2000 -> raise "no reply"
end

:ok = :clockwork.stop()

receive do
  {:clockwork_stopped, :ok} -> :ok
end
```

Then compile the module and run it against the library:

```bash
mkdir -p ebin
erlc -o ebin clockwork/src/nif/clockwork.erl
CLOCKWORK_NIF_PATH=build/nif elixir -pa ebin hello.exs
```

It prints the `/version.reply` packet. The `receive` matches only that reply,
so any `{debug, _}` lines wait in the mailbox. Without `headless: true` the
engine opens an audio device.

The NIF's own tests, in `test/nif`, are run by `scripts/test-nif.sh`; see
[Building from Source](BUILDING.md#nif-tests).
