<!-- SPDX-License-Identifier: AGPL-3.0-or-later -->
<!-- Copyright (c) 2026 Sam Aaron -->
# Security

## Reporting a vulnerability

Report it privately, through GitHub:
<https://github.com/samaaron/supersonic/security/advisories/new>,
or by email to <security@sonic-pi.net>.

Please do not open a public issue, pull request or discussion for a
vulnerability.

A useful report says:

- what is affected: the npm package, the native `supersonic` server or the
  Debian package, its version or commit, and the platform
- what an attacker needs: a SynthDef or sample file a user loads, the secret
  for a command port, a page that embeds SuperSonic
- how to reproduce it, ideally with the smallest input that does
- whether you know of it being **actively exploited**. If so, put that in the
  title, and it goes to the front of the queue.

The scsynth code SuperSonic takes from SuperCollider, and Clockwork, which
SuperSonic is built on, are in scope here too.

## What happens next

SuperSonic handles vulnerabilities as the EU Cyber Resilience Act (Regulation
(EU) 2024/2847, Annex I Part II) requires of a manufacturer, and promises
nothing beyond that:

- Your report is acknowledged, and you are told who is handling it.
- A report is assessed, and a confirmed vulnerability is fixed without undue
  delay. The fix is released as a security update, on its own rather than
  inside other changes where that is practical, and free of charge.
- Disclosure is coordinated: you tell us before you publish, and we tell you
  before we do. When the fix is out, a GitHub Security Advisory with a CVE ID
  (GitHub is a CVE Numbering Authority) says what the vulnerability was, what
  it affects, how severe it is and what to do. In a justified case publication
  waits until users have had the chance to apply the fix. The reporter is
  credited unless they ask otherwise.
- A report of active exploitation goes to the front of the queue. SuperSonic
  gives ENISA an early warning within 24 hours of becoming aware of an
  actively exploited vulnerability, a notification within 72 hours and a
  final report within 14 days of a fix.

There is no bug bounty.

## Supported versions

SuperSonic is not at 1.0 yet. A security fix goes into the next release, and
earlier releases are not updated: update to the latest release to take a fix.
Each advisory names the first fixed version.

From 1.0, each release states its support period and the date it ends.

## How it is tested

The SynthDef reader, scsynth's command handlers, a SynthDef played and the
controls of every shipped SynthDef each have a fuzz target. On every push,
each target is replayed over its corpus, the seeds and every crasher ever
found, deterministically, under AddressSanitizer and
UndefinedBehaviorSanitizer, with the real-time memory pool replaced by
`malloc` so the sanitizers see inside it. The native suite also plays every
unit generator with no inputs and with extreme ones. Every night, each target
runs under libFuzzer for two hours; a crasher is sealed to a key only the
maintainer holds before it leaves the runner, and once fixed joins the corpus.
`.github/workflows/security.yml` is the whole of it. What is not fuzzed yet is
listed in [docs/THREATS.md](docs/THREATS.md#known-gaps).

Clockwork's transports, sample decoder, audio file writer and rings are fuzzed
in Clockwork's own workflow.

## What SuperSonic contains

What SuperSonic carries besides its own code and Clockwork, and where each part
came from, is in [docs/components.cdx.json](docs/components.cdx.json). On every
push, and every night, CI writes a CycloneDX SBOM for each build target, with
Clockwork's from the pinned commit, and looks every component up for
advisories. The same documents can be written from any commit with
`node clockwork/scripts/sbom.mjs --root . target all`. See
[docs/THREATS.md](docs/THREATS.md#supply-chain).

## Scope

[docs/THREATS.md](docs/THREATS.md) says what SuperSonic defends against and
what it trusts by design. In short:

- **In scope:** anything that lets a SynthDef, a sample file or a command
  crash the process, read or write out of bounds, or run code.
- **Out of scope:** a client on a command port controlling the engine,
  including reading and writing files the process can reach (that is what the
  port is for: keep the secret); a denial of service that needs the attacker
  to already be such a client; a SynthDef using the CPU or memory its graph
  asks for, within the engine's limits; a page that embeds SuperSonic doing
  what its own code does.
