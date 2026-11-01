# Commands: one front for every path

`supersonic::Commands` (`front/supersonic_commands.h`) is what a client of
SuperSonic sends its commands through. It answers the verbs scsynth did with
files and the OS — `/b_allocRead`, `/b_allocReadChannel`, `/b_read`,
`/b_readChannel`, `/b_write`, `/d_load`, `/d_loadDir`, `/clockwork/record/*`,
`/clockwork/summary`, `/quit` — and passes everything else to the engine.
The engine has no files (clockwork's `docs/THREATS.md`): a sample is decoded
by the client into the assets area and committed as an asset; a synthdef
file becomes `/d_recv`.

This document is the contract for where `Commands` sits and what it is built
on. The rule: **every path to the engine answers the same verbs**, because
every path goes through the same `Commands`.

## Built on a client handle

`Commands` is built on a `ClockworkClient*` (`clockwork/src/clockwork_client.h`),
never on the engine object:

| who | the handle | where the front runs |
|---|---|---|
| the server (`host/SuperSonicMain.cpp`) | `clockwork_client_open_memory` over the engine's arena | in the server process, between the socket and `engine.ingest` |
| the embedded host (`host/EmbeddedHost.h`) | the same | in the embedding process |
| the NIF (`clockwork/src/nif`) | the same, through `clockwork_nif_make_front(ClockworkClient*, IOscTransport&)` | in the BEAM |
| a shared-memory peer | `clockwork_client_open_shm_peer` | in the peer's own process |

What it needs from the handle, and nothing more:

| need | call |
|---|---|
| the assets area (it writes) and the returns area (it reads) | `clockwork_client_region(ASSETS)`, `(RETURNS)` |
| send a command on an asker's behalf, or its own | `clockwork_client_send(c, bytes, n, token)` |
| the engine's block count and block size, to pace `/d_recv` batches | `clockwork_client_metrics` (the `PerformanceMetrics` words) |
| the engine's replies, to answer the asker in scsynth's words | see below |

A front is made once the engine has an arena — after `init()` for an
in-process host, after the attach for a peer — because the handle is.

## Two ways replies reach it

In-process, the front stands in the egress path as it does today
(`FrontedTransport`, `OscFront::egress`): every reply the engine sends is
offered to the front first, and the front takes the ones that answer what it
sent (`/clockwork/asset/committed`, `/b_info` for a `/b_write`, the `/done` of
its own `/b_free`) and lets the rest through. The asker's token travels with
the command and comes back on the reply, so a front with several askers
tells them apart.

In a peer process there is no transport to stand in: the peer's replies
arrive by `clockwork_client_poll` on its handle, every one stamped with the
plane's one token (the engine assigns identity by transport and trusts none
from shared memory). So the front offers its own poll:

```
Commands::poll(out, max)   // polls the handle; keeps what answers the front's own
                           // sends; returns the rest, plus the front's answers
                           // to the asker (/done "/b_allocRead" 3 …) in order
```

and keys what it is waiting for by **verb and buffer number**, not by token:
with one token there is nothing else to key by, and `/b_allocRead 3` cannot
be in flight twice for one buffer anyway. The in-process front keeps the
token as a second key, which is how a server with several sockets stays
exact.

`replies` (the `IOscTransport*` the front's own answers leave by) is therefore
optional: given, the front answers through it; not given, the answers come
out of `poll`.

## The C ABI

A peer written in another language reaches the front through a C surface,
`supersonic_commands.h`, on top of Clockwork's client ABI:

```
SupersonicCommands* supersonic_commands_open(ClockworkClient* c);      // the handle stays the caller's
int   supersonic_commands_send(SupersonicCommands*, const uint8_t* osc, uint32_t n, uint32_t token);
                                                                     // 1: taken by the front; 0: sent on
uint32_t supersonic_commands_poll(SupersonicCommands*, ClockworkClientMessage* out, uint32_t max);
void  supersonic_commands_run_pending(SupersonicCommands*);          // Work::Host
void  supersonic_commands_close(SupersonicCommands*);
```

`send` either answers (a file verb, taken and worked on the front's thread)
or writes the bytes to the handle unchanged. Nothing a peer sends reaches
the engine any other way, so the peer and the socket client are the same
client with the same verbs.

## What changes in Clockwork

- `clockwork_client_region(ASSETS)` and `(RETURNS)` on an in-process handle
  resolve the lanes from the arena's table when their space is the engine's
  process (`CLOCKWORK_SPACE_PROCESS`): the address is this process's. Today
  the call answers absent, which was true before the table named them.
- `clockwork_nif_make_front` takes the NIF's client handle instead of the
  engine.
- Nothing in the engine: the control pass still refuses nothing a peer
  sends, because the peer's front has already answered what the engine
  would not.

## What goes

- `Commands(ClockworkEngine&, …)`: the one constructor is over a handle.
- The paragraph in `NATIVE.md` about what the plane does not carry: it
  carries everything, because the peer carries it.

## Tests

- clockwork `test/test_client_abi.cpp` — an in-process handle sees the
  assets lane the engine carved, writable, and the returns lane; without
  lanes, absent.
- `test/native/test_commands.cpp` — the server's front over a memory handle
  answers `/b_allocRead` as before (the existing cases keep holding).
- `test/native/test_commands_peer.cpp` — an engine with `shmCommands`, a
  front over `clockwork_client_open_shm_peer_handles`: `/b_allocRead` of a
  shipped sample through `Commands::send` is answered `/done` through
  `Commands::poll`, and `/b_query` then reports the frames; `/d_load` the
  same; the engine never saw a file verb.
- `test/native/test_supersonic_binary.cpp` — unchanged: the binary's front
  over its own handle.
