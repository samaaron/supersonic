# Debian Packaging

SuperSonic carries everything needed to build it as a proper Debian package,
and CI proves it works: `.github/workflows/debian.yml` builds a real source
package, compiles it in a **network-disconnected** container on **trixie**
(stable), runs the full native test suite against Debian-archive dependency
versions during the build, then runs `lintian --fail-on error,warning`,
`autopkgtest`, and an install-and-boot smoke test in a pristine container.
It runs on pushes to `main` (the deb each push builds is kept, and a release
takes it from the tagged commit's run: `.github/workflows/publish.yml`), on
pull requests to `main` that
change a path it lists (the sources, the native tests, the packaging and the
man page among them), and by hand.

The goal is that a Debian maintainer can package SuperSonic with near-zero
friction: the repo demonstrates the whole pipeline rather than asking them to
discover it.

## What gets built

One binary package, `supersonic`: the native server (`/usr/bin/supersonic`,
scsynth-compatible), the plugin bridge it spawns
(`/usr/libexec/supersonic/clockwork-plugin-bridge`, out of PATH; the engine is
built knowing to look there) and a man page. The BEAM NIF, npm/web
assets, synthdefs and samples are not packaged (the latter two remain in the
source tarball because the test suite loads them).

clockwork, the substrate SuperSonic runs on, is a git submodule; the source
package carries it under `clockwork/` in the orig tarball, so the package is
the whole program and needs no separate clockwork package.

## Where things live

| Piece | Path |
|---|---|
| Debian dir (control, rules, copyright, tests…) | `packaging/debian/` |
| Source-package assembly (orig + components + patches + .dsc) | `scripts/make-debian-source.sh` |
| Phased CI driver (deps → build → lintian → autopkgtest → smoke) | `scripts/ci-debian-package.sh` |
| Workflow | `.github/workflows/debian.yml` |
| Man page | `docs/man/supersonic.1` |

`debian/` deliberately lives under `packaging/` rather than the repo root:
top-level `debian/` dirs in upstream tarballs get in real maintainers' way.
The assembly script copies it into place.

## Dependency strategy

Everything Debian ships comes from the archive; only what Debian *cannot*
supply is vendored, each with a one-line justification in `debian/copyright`:

| Dependency | Developer build | Debian build |
|---|---|---|
| clockwork (substrate) | git submodule `clockwork/` | same sources, archived into the orig tarball |
| smoothie (audio layer) | `clockwork/smoothie/` (clockwork's vendored ISC fork of JUCE 7 modules) | same in-tree sources; no system JUCE, no juceaide |
| Audio file decoders | clockwork's Rust crate `clockwork-audio-decode` (symphonia, MPL-2.0) | same, from the vendored crates — no libsndfile |
| Audio file writers | clockwork's own (`clockwork_audio_file`: WAV and FLAC) | same |
| libsamplerate (Link Audio) | `clockwork/src/vendor/libsamplerate` (0.2.2, unmodified) | `libsamplerate0-dev` (`-DCLOCKWORK_SYSTEM_SAMPLERATE=ON`); clockwork's copy is left out of the orig tarball |
| Catch2 (tests) | an installed Catch2 3, else FetchContent pin v3.5.2 | `catch2` (found automatically via `find_package`) |
| Ableton Link | FetchContent Link-4.1 + clockwork's 4 patches | **vendored** `orig-link` component tarball — Debian's `ableton-link-dev` is 3.x and lacks the patches |
| CLAP / VST3 SDKs (plugin hosting) | installed headers, else fetched at configure time | **compiled out** (`-DCLOCKWORK_PLUGINS=OFF`): Debian ships neither SDK |
| Rust crates | crates.io (`--locked`) | **vendored** `orig-rust-vendor` component tarball, built offline (`-DCLOCKWORK_CARGO_OFFLINE=ON`) |
| midir (patched fork) | `clockwork/external/midir` (cargo path dep) | same — path deps need no vendoring |
| oscpack / nova-simd | in-tree (as in Debian's own supercollider package) | same |

The switch the Debian build turns on, `CLOCKWORK_CARGO_OFFLINE`, defaults
OFF, so macOS, Windows and developer builds are untouched by it.
`CLOCKWORK_PLUGINS` is the other way round: it defaults ON, and the Debian
build turns it off.

The CMake build passes `--locked` to every cargo build it runs (the lockfile
is committed; drift errors out instead of silently rewriting it), except in
the build against the archive's own crates below, which has no lock file.
`test/transport-harness/run.sh`, which autopkgtest runs, builds its probe
client with a plain `cargo build`.

## Source package layout

Format 3.0 (quilt), three upstream tarballs (see
`packaging/debian/README.source`):

- `supersonic_<v>.orig.tar.xz` — git archive with the clockwork submodule
  archived into `clockwork/`, minus `Files-Excluded` (the Steinberg ASIO SDK
  inside clockwork, Windows-only and dual-licensed; and clockwork's copy of
  libsamplerate, since the package links the archive's)
- `supersonic_<v>.orig-link.tar.xz` — pristine Link 4.1 **with the
  asio-standalone submodule** (GitHub tag tarballs omit submodules)
- `supersonic_<v>.orig-rust-vendor.tar.xz` — `cargo vendor` for the
  committed `clockwork/rust/Cargo.lock`

The four Link patches remain single-sourced in `clockwork/external/*.patch`;
the assembly script path-shifts them under `link/` into `debian/patches/`, so
they flow through the normal quilt machinery.

`scripts/make-debian-source.sh` archives HEAD. When HEAD is the commit that
set `package.json` to `<v>` (the one `scripts/bump-version.sh` makes and
tags) the package is `<v>-1`; anything else is a snapshot,
`<v>+git<date>.<sha>-1~ci1`. CI builds the package when the commit reaches
main, before the tag exists, and the release takes that build. `<v>` is the version in `package.json`, which
between releases is the last release's, and `+git` sorts after the end of a
version in dpkg: a snapshot sorts above the `<v>-1` release it follows and
below the next one. (`~git` would sort below `<v>-1`.) The snapshot's Debian
revision, `1~ci1`, sorts below `1`.

## Building against the archive's own crates

The package above carries its crates in the source package. The Debian
archive does not take that for dependencies: a package there builds against
the archive's own crate packages (`librust-*-dev`). The same scripts assemble
that form too:

```bash
SUPERSONIC_DEB_CRATES=system scripts/make-debian-source.sh
```

What changes in the source package it writes:

- there is no `orig-rust-vendor` component;
- `debian/patches/linux-only-manifests.patch` reduces clockwork's Cargo
  manifests to Linux and removes `clockwork/rust/Cargo.lock`. cargo resolves
  every dependency a workspace names, for every platform, before it builds
  for one, so without the patch the build stops on Windows, macOS, Android
  and browser crates it would never compile. The patch is generated from the
  manifests as they are (`clockwork/scripts/linux-manifests.py`), so it
  cannot fall behind them;
- `Build-Depends` gains one `librust-<crate>-dev` for each crate clockwork
  depends on directly, from the same script, and the autopkgtest depends on
  the build dependencies, since it builds its probe from the same crates;
- `debian/build-mode.mk` tells `debian/rules` to point cargo at
  `/usr/share/cargo/registry` and to configure with
  `-DCLOCKWORK_CARGO_LOCKED=OFF`: the archive's versions are not the lock
  file's, so cargo resolves against what is installed.

Two crates stay in the tree: clockwork's midir fork, and
`clockwork/external/symphonia-bundle-flac`, the crates.io 0.6.1 with one fix
upstream has not released (`CLOCKWORK-CHANGES.md` there). A maintainer who
prefers the archive's `symphonia-bundle-flac` removes the `[patch.crates-io]`
table in `clockwork/rust/Cargo.toml`, and loses that fix until upstream
releases it.

`.github/workflows/debian-unstable.yml` builds this on unstable, by hand
(*Run workflow*). Its `crates` phase names every crate package the archive
cannot supply and stops there if one is missing.

Where it stood on 2026-11-15, run in a `debian:sid` arm64 container on a
development machine (the workflow itself has not run yet, and the container's
network stayed connected, which the workflow's does not):

- the archive lacks one crate, `gilrs`, which game controller input uses on
  Linux. The other 10 crate packages clockwork names are there, with what
  they pull in;
- with `SUPERSONIC_DEB_GAMEPAD=off`, which leaves game controller input out
  (`clockwork-gamepad` leaves the workspace and the build is configured with
  `-DCLOCKWORK_GAMEPAD=OFF`), the whole pipeline passes: the package builds,
  the native suite passes during the build (709 test cases), lintian is clean
  with warnings failing, autopkgtest passes, and the installed server boots.

So a package for the archive either waits for `gilrs` to be packaged, or
ships without game controller input until it is.

## Testing in the pipeline

- **During the build** (`debian/rules` `dh_auto_test` override): the full
  Catch2 suite (~everything except `[benchmark]`), compiled and run against
  the *Debian* Catch2 and the Debian toolchain — this is the compatibility
  proof for archive dependency versions.
- **autopkgtest**: `supersonic -v` (superficial) plus the transport harness
  (`test/transport-harness/run.sh`) against the *installed*
  `/usr/bin/supersonic` — boots headless once per transport (UDP, TCP, UDS
  stream, UDS datagram, the shared-memory command plane) and drives OSC load
  over each. The probe client (clockwork's `transport_probe` example) builds
  offline from the vendored crates, or from the archive's in a build against
  those.
- **Smoke**: a pristine container `apt install`s the .deb (resolving runtime
  deps from the archive), checks `-v` and the man page, and boots the server
  headless.

## Running it

CI: on the triggers above; the workflow's *Run workflow* button
(`workflow_dispatch`) runs it by hand. CI builds trixie only. The local
recipe below uses sid, where maintainer uploads land; use `debian:trixie` in
both `docker run` lines to build what CI builds. It needs Docker; the phases
mirror the workflow steps, and the network-disconnect between `source` and
`build` is what makes the offline proof real:

```bash
docker run -d --name deb -v "$PWD:/src" -w /src debian:sid sleep infinity
docker exec deb scripts/ci-debian-package.sh deps
docker exec deb scripts/ci-debian-package.sh builddeps
docker exec deb scripts/ci-debian-package.sh source
docker network disconnect bridge deb
docker exec deb scripts/ci-debian-package.sh build
docker network connect bridge deb
docker exec deb scripts/ci-debian-package.sh lintian
docker exec deb scripts/ci-debian-package.sh autopkgtest
docker rm -f deb
docker run --rm -v "$PWD:/src" -w /src debian:sid bash scripts/ci-debian-package.sh smoke
```

## Known friction, stated honestly

Points a prospective maintainer will care about, and where they stand:

- **The whole program is AGPL-3+** — clockwork is AGPL-3.0-or-later (or
  commercially licensed) and scsynth is GPL-3+, so the binary is AGPL. The
  `debian/copyright` stanzas record the components individually.
- **No system JUCE** — the audio layer is clockwork's in-tree `smoothie/`
  subproject (ISC, DFSG-free), so there is no JUCE build-dependency and no
  version skew.
- **No plugin hosting** — the CLAP and VST3 SDKs are fetched at configure
  time and Debian packages neither, so the Debian build compiles the plugin
  bridge's hosting out. The bridge binary is still shipped, under
  `/usr/libexec/supersonic` (`-DSUPERSONIC_BRIDGE_INSTALL_DIR`), and the
  engine is built with that directory as its second place to look
  (clockwork's `CLOCKWORK_PLUGIN_BRIDGE_DIR`).
- **Ableton Link is embedded** — Debian's `ableton-link-dev` (3.x) is too old
  and lacks four functional patches (loopback-only discovery, peer
  enumeration, monotonic commit timestamps, LinkAudio teardown race). Until
  Debian ships Link 4 and the patches are upstreamed, the component tarball
  is the honest representation.
- **Rust crates** — the default source package vendors them, which the
  archive does not take for dependencies. The package also builds against
  the archive's `librust-*-dev` packages ("Building against the archive's own
  crates" above); what stands in the way there is one crate the archive
  lacks, `gilrs`. The dependency surface is small (gilrs, socket2, alsa,
  symphonia, plus clockwork's in-tree midir fork) and the licence allow-list
  is machine-enforced by `clockwork/rust/deny.toml`.
- **Compiled synthdefs in the source tarball** — the `.scsyndef` files
  under `packages/supersonic-scsynth-synthdefs/` are compiled artifacts whose
  sclang sources live in the Sonic Pi repository (noted in
  `debian/copyright`). They exist only to feed the test suite and are not
  shipped in the .deb. If ftpmaster objects, they can move to
  `Files-Excluded` at the cost of skipping the synthdef-loading tests.
- **`fftlib.c`** — no licence statement (US government work, public domain);
  identical situation to Debian's existing `supercollider` package.
