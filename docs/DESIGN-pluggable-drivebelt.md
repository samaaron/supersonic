# DESIGN sketch — single configurable binary + pluggable drivebelt

**Status:** proposal / sketch. Not implemented.

## Goal

**One engine library, two axes, many embeddings** — *not* one privileged binary.
The engine already runs purely through the lanes C ABI (`ss_init`/`ss_tick`/…);
this adds:

- a pluggable **drivebelt** — the clock/tick source — chosen per embedding (and,
  where it makes sense, at runtime), and
- a build flag, **`SUPERSONIC_ENABLE_SYNTH`**, that includes or omits the scsynth
  component (see *Licensing* for how licence follows from linked components).

A **no-synth** (MIT-capable) engine is then consumed in whatever form a host
needs — a **NIF**, a **dylib**, or a **standalone native server** — each linking
the same engine and selecting a drivebelt. Collapsing today's separate programs
(the native server and `src/host/main.cpp` converge — a no-synth server on the
`posix-timer`/`audio-clock` belt *is* the scheduler host) is a welcome side
effect, not the point. The point is: one engine that builds synth-on or
synth-off and embeds anywhere.

## The one hard constraint (why a single binary can't be "both")

`scsynth` is GPL; JUCE is GPL-or-commercial. A binary that **links** either is
GPL — regardless of whether the code runs. So "MIT mode" cannot be a runtime
toggle of one binary: the MIT artifact must not *contain* scsynth/JUCE. That is
necessarily a **compile-time** decision. Everything else can be runtime config.

## Axes

| axis | when | options |
|---|---|---|
| **drivebelt** (tick source) | runtime (from those compiled) | `juce` · `audio-clock` · `posix-timer` · `wasm-worklet` · `manual` |
| **subsystems** | runtime | MIDI / OSC / gamepad / Link on/off, device, ports |
| **components present** | build | `SUPERSONIC_ENABLE_SYNTH` (scsynth), `SUPERSONIC_ENABLE_LINK` (Ableton Link) |

Coupling to be honest about: `posix-timer` is sleep-jitter (ms) — fine for
MIDI/OSC scheduling, **not** sample-accurate. `audio-clock` (below) closes that
gap without JUCE: it sources a hardware-locked, sample-accurate tick from the
default device **without** audio I/O. So timing precision no longer requires
linking JUCE — only actual *synthesis* needs a belt that moves audio
(`juce`/`wasm`). Belts that need a real device (`juce`, `audio-clock`) fall back
to `posix-timer` where there is none (e.g. headless CI) — a runtime decision
based on whether the device opens.

**Which belts exist is a function of which components were compiled.** The `juce`
belt links JUCE, so it's only present when scsynth/JUCE are compiled in. A build
with neither scsynth nor JUCE has `audio-clock` (the precise one), `posix-timer`,
and `manual`. So `audio-clock` is **the** precise belt whenever JUCE is absent,
which makes **miniaudio (MIT-0 / public-domain) a load-bearing dependency** of a
JUCE-free build — it's how that binary gets a sample-accurate clock without JUCE.
The commercial product config is scsynth off, Link off, `audio-clock` belt, plus
a proprietary scheduler-backend leaf (see *Licensing*).

## The drivebelt abstraction

The lanes C ABI (`ss_init` / `ss_tick` / `ss_ingress_write` / `ss_egress_*`) is
already the seam — the engine is driven entirely through it (the freestanding
target proves it runs with no JUCE). A drivebelt is just "the thing that owns the
loop, sources `now`, and calls `ss_tick` at a cadence":

```
struct Drivebelt {
    virtual ~Drivebelt() = default;
    virtual void start() = 0;   // spins the loop; each iteration: ss_tick(now, …)
    virtual void stop()  = 0;   // wakes + joins cleanly (no leftover thread)
};
```

Implementations (compiled per build; selected at startup):

- **JuceDrivebelt** — wraps today's `JuceAudioCallback`/`AudioDeviceManager`;
  sample-accurate; provides real audio I/O. Requires JUCE ⇒ only in a GPL build.
- **AudioClockDrivebelt** — opens the **default audio device purely as a clock**:
  each device callback advances `now` by `frames / sampleRate` and calls
  `ss_tick`; the buffer is ignored/zeroed — **no audio in or out**. Sample-accurate,
  hardware-locked cadence, and **MIT**, via a permissive cross-platform audio lib
  (recommended: miniaudio — single-header, public-domain/MIT-0; CoreAudio / WASAPI
  / ALSA / PulseAudio / JACK). This is the precise MIT clock: the timing of `juce`
  with no JUCE and no audio. Needs a working device; falls back to `posix-timer`
  where none exists.
- **PosixTimerDrivebelt** — wall-clock timer thread (today's `src/host/clock.h`);
  no audio, no JUCE, no device; ms precision. MIT-capable; the headless fallback.
- **WasmDrivebelt** — the AudioWorklet `process()` path (WASM only).
- **ManualDrivebelt** — caller pumps `ss_tick` (tests, freestanding, embeds).

`stop()` must terminate its thread cleanly with no process-global leftovers —
see the nif-linux teardown hang ([nif-linux-teardown-hang handoff]); whatever the
belt starts, the belt joins.

## The standalone native server (one consumer of the engine)

One `main` that subsumes both the native `Main.cpp` and `src/host/main.cpp` — it
is *a* consumer of the engine library, alongside the NIF / dylib / WASM, not a
privileged "the binary":

```
1. parse config (CLI / env / OSC): drivebelt=…, midi/osc/gamepad/link on/off, port, …
2. build the engine (synth presence is compile-time; see flag)
3. construct the selected Drivebelt   (reject a belt the build didn't include)
4. wire transport (ingress → ss_ingress_write; ss_egress_* → transport)
5. drivebelt.start();  … ;  drivebelt.stop()
```

## Build flag: `SUPERSONIC_ENABLE_SYNTH` (default ON)

- **ON** → compile `SCSYNTH_*_SOURCES`; register the synth leaf as the **last**
  route; render the graph each block; link JUCE. Binary is **GPL**.
- **OFF** → omit scsynth sources; the synth route is simply **not registered**, so
  unrouted synth verbs fall to the unknown-endpoint log; **no per-block render**;
  **don't link JUCE**. The `audio-clock`/`posix-timer`/`manual` belts are
  available (all MIT). Binary is **MIT-capable** (same source, no GPL linked) — and
  via `audio-clock` still gets a sample-accurate clock.

This makes synth a build option **peer to** `SUPERSONIC_ENABLE_MIDI/GAMEPAD/OSC`
(today `SCSYNTH_*_SOURCES` is unconditional — this flag is the new guard).

**Dispatch: synth is just a route, the default is "log unknown endpoint", and
order matters.** Today the engine wires synth as the *catch-all default*
(`setDefault(&ss_synth_default_route)`), which masks the unknown-endpoint
diagnostic that already exists in `dispatch()` (`"no backend for OSC <addr> —
dropped"`, rate-limited). The model instead: register concrete routes
(`/midi/`, `/osc/`, `/clock/`, …) and synth **last / lowest precedence**; the
terminal default is the unknown-endpoint log, not synth. Then `ENABLE_SYNTH=OFF`
needs no special default — synth's route is absent and its verbs log as unknown.
*Note:* `OscIngress` is currently **longest-prefix, single-dispatch** (one catch-all
`mDefault`), not an ordered chain — so realising "synth last + log-unknown default"
is a small ingress change (an ordered/last-resort tier that falls through to the
log-unknown terminal), after which synth is genuinely the last-registered route.

So the only **real** synth-specific machinery the flag guards is the audio
core — *not* the dispatch:

- **CMake:** `SCSYNTH_SERVER/COMMON/PLUGIN_SOURCES` + the scsynth-coupled
  `buffer_commands.cpp` / `node_tree.cpp` become conditional. (`oscpack` stays —
  OSC parse/build is used by non-synth paths too.)
- **`audio_processor.cpp`:** the `World` lifecycle (`EngineCore_New`,
  `World_SetSampleRate`, `World_Cleanup`, `World_UpdateNativeStats`) and the
  per-block **render** (`EngineCore_BeginBlock`, `static_audio_bus`,
  `g_world->mBufLength`). No-synth `ss_tick` = drain ingress + fire scheduler,
  no render.
- **route registration:** register the synth route last (ON) or not at all (OFF);
  the terminal default is the existing unknown-endpoint log either way.
- **`SupersonicEngine.cpp` / `node_tree` / `buffer_commands`:** their `World`
  references.

The coupling is **concentrated** (World + render in `audio_processor.cpp`, plus
`buffer_commands`/`node_tree`) — not smeared — so a bounded, clean first spike.
Pair with making the JUCE link conditional (freestanding already proves a
JUCE-free engine).

## How today's targets collapse into the model

| today | becomes |
|---|---|
| `SuperSonic` exe (JUCE + synth) | the single binary, built `ENABLE_SYNTH=ON`, run `--drivebelt=juce` |
| `supersonic-scheduler` host | the **same** binary, built `ENABLE_SYNTH=OFF`, run `--drivebelt=audio-clock` (precise, MIT) or `posix-timer` (headless) — no separate program |
| `supersonic_freestanding` | `ManualDrivebelt` (or a freestanding belt) on the engine lib |
| WASM | `WasmDrivebelt` = the AudioWorklet `process()` — already the tick source; in no-synth mode it still ticks (drain ingress + fire scheduler), just renders no audio |
| ESP32 | timer/loop belt on the engine lib |
| NIF (`.so`) | a library embedding (not a `main`): embeds engine + a belt; still picks a belt, still build-gated by `ENABLE_SYNTH` |

So the **drivebelt abstraction** unifies *all* embeddings; the **single `main`**
specifically merges the native exe and the scheduler host into one configurable
program. `src/host/main.cpp` and the `SUPERSONIC_BUILD_SCHEDULER_HOST` target go
away; "scheduler host" = `ENABLE_SYNTH=OFF` + `--drivebelt=posix-timer`.

## Feasibility (is it implementable?)

Yes. By piece, easiest → hardest:

1. **Drivebelt abstraction — low risk.** The lanes ABI is already the seam, and
   every target today *already has* a de-facto belt (JUCE callback, host posix
   timer, WASM `process()`, freestanding/esp32 manual loops). A `start()/stop()`
   interface is mostly refactoring existing driver code into one shape. WASM is
   untouched: `process()` stays the belt, synth-on or off.
2. **`audio-clock` belt — low risk, small new code.** miniaudio exists for exactly
   this; only per-backend validation is opening a device clock-only.
3. **`SUPERSONIC_ENABLE_SYNTH=OFF` — the real work and the gate.** Today scsynth
   is unconditional and the core *assumes* it: the render step (`EngineCore_BeginBlock`
   + World render in `ss_tick`), the **default dispatch route**
   (`ss_synth_default_route` → `PerformOSCBundle`), bundle/buffer/node-tree
   handling, and Link-Audio bus routing all reference scsynth. No-synth =
   guard `SCSYNTH_*_SOURCES` out, supply a no-synth default route (unrouted →
   drop/error), skip the render in `ss_tick`, `#ifdef` the touchpoints. Moderate,
   careful refactor — and **not yet proven**: freestanding compiles JUCE-free but
   still *includes* scsynth, so the no-synth path is unexercised. **De-risk this
   first** with a spike: get `supersonic_engine` to compile+link with
   `ENABLE_SYNTH=OFF` before wiring belts/consumers.
4. **JUCE optional — low/moderate.** Precedent exists (freestanding links no JUCE
   via the ABI); make the JUCE link conditional, with non-JUCE belts
   (`audio-clock`/`posix-timer`) for no-synth.
5. **Consumer packagings — low.** NIF and the native exe already embed the engine;
   no-synth variants are the same targets built `ENABLE_SYNTH=OFF` + an MIT belt.
   WASM no-synth = `process()` belt + the flag. The dylib is a thin new target over
   the existing lanes ABI.

**Net:** items 1–2, 4–5 largely formalize what already exists. The one
substantial, unproven piece is cleanly excising scsynth behind `ENABLE_SYNTH`
(item 3) — that's the gate; spike it first.

## Licensing

A build's licence is the union of what it links. GPL-encumbered components:
scsynth (GPL), JUCE and Ableton Link (GPL-or-commercial). First-party code is
`MIT OR GPL-3.0-or-later`; the deps of a no-scsynth/no-JUCE build are permissive
(Rust crates allow-listed in `rust/deny.toml`, miniaudio MIT-0, oscpack/tlsf). So
a build linking none of {scsynth, JUCE, Ableton Link} is MIT-licensable. A CI
symbol check (`nm`/`ar`) enforces it; audit the full dep tree — one LGPL/GPL
transitive dep defeats it.

## Non-goals / open questions

- **Not** a single binary that is both GPL and MIT — impossible; `ENABLE_SYNTH`
  is the compile switch. The win is *one source / one main*, not one artifact.
- Config surface: CLI flags vs env vs an OSC `/config` verb — pick one (CLI for
  the standalone main is simplest).
- A belt requested but not compiled in → hard error at startup. Device-needing
  belts (`juce`, `audio-clock`) that fail to *open* at runtime → fall back to
  `posix-timer` (this is the headless-CI path).
- `audio-clock` adds one small MIT dependency (miniaudio, single-header — no build
  system impact). It opens a minimal default-device stream and writes silence
  (or a no-output/loopback mode where the backend allows); the only thing
  consumed is the callback cadence. Confirm each backend can open a stream with
  zero meaningful I/O without forcing a real output.
- Migration order: (1) extract `Drivebelt` interface over the lanes ABI;
  (2) wrap the current JUCE callback as `JuceDrivebelt` and `src/host` as
  `PosixTimerDrivebelt`; (3) merge the two `main`s; (4) add `SUPERSONIC_ENABLE_SYNTH`
  + conditional JUCE link; (5) delete `src/host/main.cpp` and the host CMake target.

## Related

- `docs/DESIGN-unified-ingress-egress.md` — the token-in/token-out engine seam
  the drivebelt drives.
- lanes ABI: `src/lanes/lanes.h` (`ss_init`/`ss_tick`/`ss_ingress_write`/`ss_egress_*`).
