<!-- SPDX-License-Identifier: AGPL-3.0-or-later -->
<!-- Copyright (c) 2026 Sam Aaron -->
# Security policy

## Reporting a vulnerability

Please report it privately, through GitHub:
<https://github.com/samaaron/supersonic/security/advisories/new>,
or by email to <security@sonic-pi.net>.

Please don't open a public issue, pull request or discussion for a
vulnerability.

A useful report tells us:

- what is affected: the npm package, the native `supersonic` server or the
  Debian package, its version or commit, and the platform
- what an attacker needs: a SynthDef or sample file a user loads, or the
  secret for a command port
- how to reproduce it, ideally with the smallest input that does
- whether you know of it being **actively exploited**. If so, put that in the
  title and we'll deal with it first.

You can report vulnerabilities in Clockwork, which SuperSonic is built on,
here too.

## What counts as a vulnerability

[docs/THREATS.md](docs/THREATS.md) describes what SuperSonic defends against
and what it trusts by design.

We treat as a vulnerability anything that lets a SynthDef, a sample file or a
command crash the engine, read or write memory it shouldn't, or run code.

We don't treat these as vulnerabilities:

- Anything a client can do once it has the server's secret, such as
  controlling the engine, overloading it, shutting it down with `/quit`, or
  reading and writing files the server can reach. That's what the command
  port is for, so keep the secret safe.
- A SynthDef that uses a lot of CPU or memory, as long as it stays within the
  engine's limits.

## What happens after you report

We'll acknowledge your report, keep you updated while we fix it, and agree with
you when to make it public. We'll credit you unless you'd rather we didn't.
There is no bug bounty.

## Which versions get fixes

SuperSonic is not at 1.0 yet. We put a security fix in the next release and
don't update earlier ones, so update to the latest release to get a fix.

From 1.0, each release will say how long we support it.

## How we test SuperSonic

We fuzz four parts of SuperSonic:

- the SynthDef reader
- playing a SynthDef
- scsynth's command handlers
- the controls of every SynthDef we ship

On every push, CI replays each fuzz target over its corpus (its seed inputs
and every input that has ever crashed it) under AddressSanitizer and
UndefinedBehaviorSanitizer. For these runs we replace the real-time memory
pool with `malloc`, so the sanitizers can see out-of-bounds reads and writes
inside it.

Every night, CI fuzzes each target with libFuzzer for two hours. Before it
uploads a crashing input, it encrypts it to a key only the maintainer has.
When we fix the bug, we add the input to the corpus, so every push replays it
from then on.

The native test suite also plays every unit generator with no inputs, with
extreme input values and with control-rate inputs.

It's all in `.github/workflows/security.yml`. What we don't fuzz yet is listed
in [docs/THREATS.md](docs/THREATS.md#known-gaps).

Clockwork fuzzes its own transports, sample decoder, audio file writer and
ring buffers in its own workflow.

## Third-party code and SBOMs

Besides our own code and Clockwork, SuperSonic includes a few third-party
libraries: nova-simd and two files from sc3-plugins.
[docs/components.cdx.json](docs/components.cdx.json) lists each one and the
upstream commit it came from. It also records SuperCollider, which we forked
SuperSonic's synthesis engine from, as SuperSonic's ancestor.

From 0.93.0, each release includes a software bill of materials (SBOM) in
CycloneDX format for each build target, with Clockwork's alongside. CI also
writes them on every push and every night, and checks every component in them
against the OSV and NVD vulnerability databases.

To write them yourself from any commit, run:

`node clockwork/scripts/sbom.mjs --root . target all`

There is more detail in [docs/THREATS.md](docs/THREATS.md#supply-chain).
