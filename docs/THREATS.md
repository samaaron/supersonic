<!-- SPDX-License-Identifier: AGPL-3.0-or-later -->
<!-- Copyright (c) 2026 Sam Aaron -->
# Threat model

Where untrusted bytes enter and leave SuperSonic, what checks them, and what is
not covered yet. SuperSonic is a scsynth-compatible synth powered by Clockwork:
its synthesis code, forked from SuperCollider's scsynth, runs as a guest inside
[Clockwork](../clockwork/docs/THREATS.md), which owns the transports, the
per-boot secret, the rings, the clock, the devices, sample decoding and audio
file writing. This document covers what SuperSonic adds on top; Clockwork's
covers everything below.

## Who is trusted

| Party | Trust | Why |
|---|---|---|
| The embedding host (SuperSonic's native host, a page using the npm package, the NIF's BEAM) | full | it is the process |
| A client on the command transport | **control of the engine** | it can create and free synths, load SynthDefs, read and write buffers and files the process can reach. Every command port admits only a client whose first frame is the host's per-boot secret (Clockwork); the native host reads the secret from stdin |
| A SynthDef file | **none** | SynthDefs are shared between users and downloaded from the web; loading one runs a graph the file describes |
| A sample file | **none** | any file a user or client names |

A *control* client is trusted to drive the engine, but a malformed command or
SynthDef must not corrupt memory or run code.

## Every seam, in and out

Risk levels: *high* — reachable with untrusted content and no other
authority; *control* — reachable only by an authenticated client, which
already controls the engine; *low* — reachable only by the host or an
already-trusted process.

| Seam | Direction | Reached by | Code | Language | What defends it | Proved by | Risk |
|---|---|---|---|---|---|---|---|
| SynthDef bytes | in | `/d_recv` from a client; `/d_load` and `/d_loadDir` from files a client names; `loadSynthDef()` on the web | `dsp/scsynth/synth/server/SC_GraphDef.cpp` (`GraphDef_Recv`, `GraphDefLib_Read`), behind `ReadWriteMacros.h` | C++ (forked from SuperCollider) | every read checks the buffer's end and throws; a version 3 def's size is checked against what was sent; every count is checked against the bytes that follow before anything is allocated from it, and the guest's array allocator refuses a size that overflows; every unit input must name a constant or an earlier unit's output; the receive catches the throw and refuses the def | a truncated header, random bytes and a truncated def of each format version (`test/osc_semantic.spec.mjs`, `test/synthdef_versions.spec.mjs`); counts no def could hold, on the 32-bit web engine too (`test/native/test_synthdef_counts.cpp`, `test/synthdef_versions.spec.mjs`); inputs that name no source (`test/native/test_synthdef_inputs.cpp`); fuzz target `test/fuzz/fuzz_synthdef.cpp` | high: SynthDefs come from strangers |
| scsynth commands | in | a client | the 70 command handlers in `dsp/scsynth/synth/server/SC_MiscCmds.cpp` and `SC_SequencedCommand.cpp`, argument reading in `sc_msg_iter` | C++ (forked from SuperCollider) | each handler reads its arguments through `sc_msg_iter`, which stops where the type tags or the data end, reads nothing that would pass the end (a short value, a blob longer than the rest), reads a string the data does not hold as empty, and holds a float read as an int to the int's range; the control array loops end with the message and pass over a type no control takes; node, buffer and bus indices are checked against the World's limits, and the buffer commands that run in stages refuse a buffer number the World has no buffer for before any stage runs; a count in /n_fill, /n_mapn and /n_mapan loops no further than the controls under the node, and the getn commands check every count before reserving their reply's tags | the OSC suites (`test/osc_semantic.spec.mjs`, `test/native/test_osc_commands.cpp`, `test_osc_semantic.cpp`); the reader over messages held in buffers of exactly their size (`test/native/test_sc_msg_iter.cpp`); arrays never closed, run short or holding other types (`test/native/test_osc_arrays.cpp`); buffer numbers out of range (`test/native/test_buffer_alloc_bounds.cpp`); counts of INT32_MAX and negative counts (`test/native/test_osc_counts.cpp`); fuzz target `test/fuzz/fuzz_scsynth_commands.cpp`, which leaves out the verbs that read or write files, quit, or are Clockwork's | control |
| Unit generator controls | in | a client, through `/s_new`, `/n_set`, `/c_set` and the bus and buffer commands | the UGens in `dsp/scsynth/synth/plugins` | C++ (forked from SuperCollider) | each UGen's own range handling (buffer numbers, indices, sizes) | the UGen suites; no fuzz target | control |
| Sample files, read | in | `/b_allocRead` and the other buffer reads, from files a client names | `front/supersonic_commands.cpp` (`decodeFor`: start, frame count and channel picks applied to the decoded file) over Clockwork's decoder | C++ glue over safe Rust | the decoder is Clockwork's (fuzzed there); the glue clamps the start and frame count to the decoded file and refuses a channel the file does not have | `test/native/test_buffer_*`; the glue is not fuzzed | high: files come from untrusted sources |
| Sample files, read (web) | in | `loadSample()` on a page | the browser's own decoder (`decodeAudioData`), and `js/lib/aiff_converter.js` for AIFF | the browser; JavaScript | memory-safe; a malformed file is a rejected promise | `test/load_sample.spec.mjs` | low |
| SynthDef names (web) | in | `loadSynthDef()` on a page | `js/lib/synthdef_parser.js` (reads the def's name before it is sent) | JavaScript | memory-safe; a malformed def throws | the web suites | low |
| Files written | out | `/b_write` and recording, to paths a client names | `front/supersonic_commands.cpp`, `front/Recorder.cpp`, over Clockwork's writer | C++ glue | the path is the client's: a client can write anywhere the process can, by design | `test/native/test_front_recording.cpp`; the writer is fuzzed in Clockwork | control |
| Replies | out | the requesting client, or every client registered with `/notify` | scsynth's reply encoders (`SC_Reply`, `SendReply`, `/n_go` and the other notifications) through Clockwork's egress | C++ (forked from SuperCollider) | every reply any native case receives is decoded by the client parser (Clockwork's fixture check) | Clockwork's `clockwork_osc_decodes` check in the fixtures SuperSonic's native suite shares | low |
| The native host's start-up | in | the process that starts it | `host/SuperSonicMain.cpp` (options, the secret on stdin) | C++ | the secret must be 64 hex characters and a newline before any port opens, or the host exits | `test/native/test_supersonic_binary.cpp`, the transport harness | low |
| The NIF | in | the BEAM that loads it | `front/supersonic_nif_commands.cpp` over Clockwork's NIF | C++ | the BEAM is the host | the NIF build | low |

Each fuzz target is replayed over its corpus on every push and fuzzed for two
hours every night (`.github/workflows/security.yml`).

## Known gaps

Open items, most important first.

1. **A SynthDef is read under the fuzzer but never played.** A unit
   generator reads the inputs its code expects, and the reader does not check
   a unit's input count against its unit generator, as upstream does not.
2. **The UGens trust their controls as upstream does.** A control value that
   names a buffer or an index is checked by each UGen; nothing exercises them
   with hostile values systematically.
3. **The buffer-read glue is not fuzzed.** `decodeFor` applies a client's
   start, frame count and channel picks to a decoded file.

## Supply chain

`docs/components.cdx.json` lists what SuperSonic carries besides its own code
and Clockwork: the SuperCollider code its synthesis was forked from, by the
upstream commit SuperSonic last took changes from; nova-simd, TLSF and two
sc3-plugins files, each by upstream commit; and Catch2, which the tests fetch.
The npm packages come from `package-lock.json`. `node clockwork/scripts/sbom.mjs
--root . check` holds the list to the tree, `target all` writes one CycloneDX
SBOM per build target with Clockwork's from the pinned commit beside it, and
`scan` looks every component up in OSV or the NVD. `.github/workflows/security.yml`
runs all three on every push and nightly; an advisory fails the run unless a
statement in `docs/vex.cdx.json` or Clockwork's accepts it.
