# Debian Packaging

SuperSonic carries everything needed to build it as a proper Debian package,
and CI proves it works on every push: `.github/workflows/debian.yml` builds a
real source package, compiles it in a **network-disconnected** container on
both **trixie** (stable) and **sid** (unstable), runs the full native test
suite against Debian-archive dependency versions during the build, then runs
`lintian --fail-on error,warning`, `autopkgtest`, and an install-and-boot
smoke test in a pristine container.

The goal is that a Debian maintainer can package SuperSonic with near-zero
friction: the repo demonstrates the whole pipeline rather than asking them to
discover it.

## What gets built

One binary package, `supersonic`: the native server (`/usr/bin/supersonic`,
an scsynth drop-in) plus a man page. The BEAM NIF, npm/web assets, synthdefs
and samples are not packaged (the latter two remain in the source tarball
because the test suite loads them).

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
| JUCE | FetchContent pin 8.0.13 | `juce-modules-source` + `juce-tools` (8.0.6+ds) via `-DSUPERSONIC_SYSTEM_JUCE=ON` |
| libsndfile + ogg/vorbis/flac/opus | FetchContent static stack | `libsndfile1-dev` (shared, codecs included) via `-DSUPERSONIC_SYSTEM_SNDFILE=ON` |
| Boost (header-only subset) | in-tree bcp extract of 1.86 | `libboost-dev` via `-DSUPERSONIC_SYSTEM_BOOST=ON` |
| Catch2 (tests) | FetchContent pin v3.5.2 | `catch2` (found automatically via `find_package`) |
| Ableton Link | FetchContent Link-4.0 + 4 patches | **vendored** `orig-link` component tarball — Debian's `ableton-link-dev` is 3.x and lacks the patches |
| Rust crates | crates.io (`--locked`) | **vendored** `orig-rust-vendor` component tarball, built `--locked --offline` |
| midir (patched fork) | in-tree `external/midir` (cargo path dep) | same — path deps need no vendoring |
| tlsf / oscpack / nova-simd | in-tree (as in Debian's own supercollider package) | same |

The relevant CMake switches are all independent and default OFF, so
macOS/Windows/developer builds are untouched. `--locked` is now unconditional
for every cargo invocation (the lockfile is committed; drift errors out
instead of silently rewriting it).

## Source package layout

Format 3.0 (quilt), three upstream tarballs (see
`packaging/debian/README.source`):

- `supersonic_<v>.orig.tar.xz` — git archive minus `Files-Excluded`
  (currently only the Steinberg ASIO SDK: Windows-only, and its directory
  carries non-free PDFs/logo artwork)
- `supersonic_<v>.orig-link.tar.xz` — pristine Link 4.0 **with the
  asio-standalone submodule** (GitHub tag tarballs omit submodules)
- `supersonic_<v>.orig-rust-vendor.tar.xz` — `cargo vendor` for the
  committed `rust/Cargo.lock`

The four Link patches remain single-sourced in `external/*.patch`; the
assembly script path-shifts them under `link/` into `debian/patches/`, so
they flow through the normal quilt machinery.

Snapshot builds are versioned `<v>+git<date>.<sha>-1~ci1` so they sort below
the eventual `<v>-1` release.

## Testing in the pipeline

- **During the build** (`debian/rules` `dh_auto_test` override): the full
  Catch2 suite (~everything except `[benchmark]`), compiled and run against
  the *Debian* versions of JUCE/libsndfile/Boost/Catch2 — this is the
  compatibility proof for archive dependency versions.
- **autopkgtest**: `supersonic -v` (superficial) plus the transport harness
  (`test/transport-harness/run.sh`) against the *installed*
  `/usr/bin/supersonic` — boots headless once per transport (UDP, TCP, UDS
  stream, UDS datagram) and drives OSC load over each. The probe client
  builds offline from the vendored crates.
- **Smoke**: a pristine container `apt install`s the .deb (resolving runtime
  deps from the archive), checks `-v` and the man page, and boots the server
  headless.

## Running it

CI: every push/PR, or manually via the workflow's *Run workflow* button
(`workflow_dispatch`). Locally (needs Docker; the phases mirror the workflow
steps — network-disconnect between `source` and `build` is what makes the
offline proof real):

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

- **JUCE version skew** — the developer pin is 8.0.13; Debian ships 8.0.6+ds.
  CI builds and runs the full test suite against 8.0.6 on every push, so
  incompatibility would surface here first, not on a maintainer's machine.
- **Ableton Link is embedded** — Debian's `ableton-link-dev` (3.x) is too old
  and lacks four functional patches (loopback-only discovery, peer
  enumeration, monotonic commit timestamps, LinkAudio teardown race). Until
  Debian ships Link 4 and the patches are upstreamed, the component tarball
  is the honest representation.
- **Rust crates are vendored** — accepted Debian practice for applications,
  but an archive maintainer may prefer `librust-*-dev` packages + `dh-cargo`.
  The dependency surface is small (gilrs, socket2, alsa, plus the in-tree
  midir fork) and the licence allow-list is machine-enforced by
  `rust/deny.toml`.
- **Compiled synthdefs in the source tarball** — the 131 `.scsyndef` files
  under `packages/supersonic-scsynth-synthdefs/` are compiled artifacts whose
  sclang sources live in the Sonic Pi repository (noted in
  `debian/copyright`). They exist only to feed the test suite and are not
  shipped in the .deb. If ftpmaster objects, they can move to
  `Files-Excluded` at the cost of skipping the synthdef-loading tests.
- **`fftlib.c`** — no licence statement (US government work, public domain);
  identical situation to Debian's existing `supercollider` package.
