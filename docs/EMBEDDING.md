# Embedding SuperSonic in an application

SuperSonic is clockwork hosting scsynth, and clockwork is a library: it opens
no socket, parses no command line and runs no event loop. The `supersonic`
server is one process built around it (`host/SuperSonicMain.cpp`); the NIF is
a BEAM binding around the same pieces. This document is the third shape: the
engine inside an application's own process, which is how Sonic Pi's desktop
app runs it from v6.

## The pieces

| piece | where | what it is |
|---|---|---|
| `ClockworkEngine` | `clockwork/src/native/ClockworkEngine.h` | the engine: devices, clock, Link, MIDI, OSC, gamepads, the rings, the segment |
| `supersonic::Commands` | `front/supersonic_commands.h` | the front: scsynth's file verbs (`/b_allocRead`, `/d_load`, `/d_loadDir`, `/b_write` …) and `/clockwork/record/*`, answered by reading and writing the files as a client; the engine stays file-free |
| `IOscTransport` | `clockwork/src/native/IOscTransport.h` | where the engine's replies and broadcasts go; the server gives it sockets, the NIF the BEAM, an application a callback |
| `supersonic::EmbeddedHost` | `host/EmbeddedHost.h` | the three above, composed, with one callback for every reply |

An application links `supersonic_embed` (this repository's CMake target) and
includes `EmbeddedHost.h`:

```cpp
#include "EmbeddedHost.h"
#include "scsynth_options_text.h"

supersonic::EmbeddedHost host;

ClockworkEngine::Config cfg;
cfg.udpPort   = 57110;               // > 0: a shared-memory segment for scopes, metrics, the node tree.
                                     // Nothing is bound: the number only names the segment.
cfg.appName   = "My App";            // what the OS shows (PipeWire nodes, Link peers, MIDI clients)
cfg.defaultBpm = 60.0;
cfg.hardwareDevice = "";             // "" = the default output; a device by name otherwise
cfg.pluginBridgePath = bridgePath;   // where YOU put the plugin bridge (plugin builds); "" = beside the executable
cfg.inboxBytes = size_t(512) << 20;  // the lane a sample is staged in on its way to the guest: it must hold
                                     // the largest sample you will load, as floats. Address space, not memory
                                     // (pages commit as written); the default, 8 MB, is a few seconds of audio

ScsynthOptions o;                    // the guest's own tables, as a struct
scsynth_options_defaults(&o);
o.numAudioBusChannels = 1024;
o.realTimeMemorySize  = 131072;      // KB
cfg.guestConfig = scsynth_options_text(o);

supersonic::EmbeddedHost::Callbacks cb;
cb.onMessage = [](const uint8_t* osc, uint32_t n) { /* a reply or broadcast: decode, hand on */ };
cb.onLog     = [](const std::string& line) { /* the engine's log */ };

std::string err;
if (!host.boot(cfg, cb, &err)) fail(err);
host.loadDefinitions("/path/to/synthdefs", &err);   // scsynth's -D, blocking

host.send(bytes, n);                 // any OSC message or bundle, as to scsynth over UDP
...
host.shutdown();
```

Everything the engine answers — `/done`, `/fail`, `/n_go`, `/status.reply`,
and every `/clockwork/*` push once subscribed (`/clockwork/notify`,
`/clockwork/midi/notify/subscribe`, `/clockwork/clock/notify/subscribe`,
`/clockwork/osc/notify/subscribe`) — arrives on `onMessage`. The OSC surface is
[OSC_API.md](OSC_API.md), the same as over a socket.

## Threads

The rules the rings impose, in one place. `EmbeddedHost.h` repeats them.

| call | thread |
|---|---|
| `boot()`, `shutdown()` | the calling thread, blocking for seconds (the device opens; definitions load). Not the audio thread, not a thread that must stay responsive |
| `send()` | any thread but the audio thread. A ring write, microseconds; serialised inside |
| `onMessage` | the engine's NRT gateway thread, one thread, in order. Decode and hand on; `send()` from it is fine; `boot()` and `shutdown()` are not |
| `onLog` | any of the engine's threads, the audio thread included (lock-free there, may drop). Nothing slow |
| `loadDefinitions()` | the calling thread, blocking until the engine has answered for each file. After `boot()` |
| `dupSegmentHandle()` | any thread. The handle is a reader's to own (`shm_segment_client`) |

One engine per process: clockwork is a process singleton.

## Reading the segment in-process

With `cfg.udpPort > 0` the engine publishes its shared-memory segment —
metrics, the scope streams, the audio taps, the guest's node-tree window —
exactly as the server does for a GUI in another process. In-process the
reader needs no attach socket:

```cpp
shm_segment_client reader(host.dupSegmentHandle());   // owns the duplicate
auto scope = reader.get_scope_stream_reader(0);
```

A reader re-attaches after a cold swap (`/clockwork/setup` on `onMessage`):
the engine keeps one segment across swaps, but a reader's view of the
guest's regions is rebuilt.

## The plugin bridge

Plugins run in clockwork's bridge, a process the engine spawns and watches
([clockwork/docs/TRACKS.md](../clockwork/docs/TRACKS.md), "The bridge"). The
engine looks for it, in order: `cfg.pluginBridgePath`, the
`CLOCKWORK_PLUGIN_BRIDGE` environment variable, beside the process's own
executable, then `CLOCKWORK_PLUGIN_BRIDGE_DIR`. An application whose binary is
not the engine's names the bridge in its config: its layout is its own. On
macOS the bridge is an app bundle and the path is the executable inside it,
`<name>.app/Contents/MacOS/<name>`.

## What this is not

- Not the web: the AudioWorklet has no threads to give, and the JavaScript
  client (`js/supersonic.js`) is the host there.
- Not `clockwork_embed.h`, clockwork's C API for a host that renders the
  blocks itself (a DAW plugin). `EmbeddedHost` lets the engine open the device
  and tick itself, and stands SuperSonic's front in front of it.
