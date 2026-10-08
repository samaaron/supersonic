# Debian Packaging

SuperSonic carries everything needed to build it as a proper Debian package,
and CI proves it works: `.github/workflows/debian.yml` builds a real source
package, compiles it in a **network-disconnected** container on **trixie**
(stable), runs the full native test suite against Debian-archive dependency
versions during the build, then runs `lintian --fail-on error,warning`,
`autopkgtest`, and an install-and-boot smoke test in a pristine container.
It runs on pushes to `main`, on `v*` tags, on pull requests to `main` that
change a path it lists (the sources, the native tests, the packaging and the
man page among them), and by hand.

The goal is that a Debian maintainer can package SuperSonic with near-zero
friction: the repo demonstrates the whole pipeline rather than asking them to
discover it.

## What gets built

One binary package, `supersonic`: the native server (`/usr/bin/supersonic`,
an scsynth drop-in), the plugin bridge it spawns
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
| zlib (inside smoothie) | vendored copy | `zlib1g-dev` via `-DCLOCKWORK_SYSTEM_ZLIB=ON` |
| stb_vorbis (inside clockwork) | vendored copy (v1.22, unmodified) | `libstb-dev` via `-DCLOCKWORK_SYSTEM_STB=ON` |
| Audio file codecs | clockwork's own (`clockwork_audio_file`, dr_libs, flac encoder) | same — no libsndfile; dr_libs has no distro package |
| Catch2 (tests) | an installed Catch2 3, else FetchContent pin v3.5.2 | `catch2` (found automatically via `find_package`) |
| Ableton Link | FetchContent Link-4.1 + clockwork's 4 patches | **vendored** `orig-link` component tarball — Debian's `ableton-link-dev` is 3.x and lacks the patches |
| CLAP / VST3 SDKs (plugin hosting) | installed headers, else fetched at configure time | **compiled out** (`-DCLOCKWORK_PLUGINS=OFF`): Debian ships neither SDK |
| Rust crates | crates.io (`--locked`) | **vendored** `orig-rust-vendor` component tarball, built offline (`-DCLOCKWORK_CARGO_OFFLINE=ON`) |
| midir (patched fork) | `clockwork/external/midir` (cargo path dep) | same — path deps need no vendoring |
| tlsf / oscpack / nova-simd | in-tree (as in Debian's own supercollider package) | same |

The switches the Debian build turns on — `CLOCKWORK_SYSTEM_ZLIB`,
`CLOCKWORK_SYSTEM_STB`, `CLOCKWORK_CARGO_OFFLINE` — are independent and
default OFF, so macOS, Windows and developer builds are untouched by them.
`CLOCKWORK_PLUGINS` is the other way round: it defaults ON, and the Debian
build turns it off.

The CMake build passes `--locked` to every cargo build it runs (the lockfile
is committed; drift errors out instead of silently rewriting it).
`test/transport-harness/run.sh`, which autopkgtest runs, builds its probe
client with a plain `cargo build`.

## Source package layout

Format 3.0 (quilt), three upstream tarballs (see
`packaging/debian/README.source`):

- `supersonic_<v>.orig.tar.xz` — git archive with the clockwork submodule
  archived into `clockwork/`, minus `Files-Excluded` (currently only the
  Steinberg ASIO SDK inside clockwork: Windows-only, dual-licensed)
- `supersonic_<v>.orig-link.tar.xz` — pristine Link 4.1 **with the
  asio-standalone submodule** (GitHub tag tarballs omit submodules)
- `supersonic_<v>.orig-rust-vendor.tar.xz` — `cargo vendor` for the
  committed `clockwork/rust/Cargo.lock`

The four Link patches remain single-sourced in `clockwork/external/*.patch`;
the assembly script path-shifts them under `link/` into `debian/patches/`, so
they flow through the normal quilt machinery.

`scripts/make-debian-source.sh` archives HEAD. When HEAD is tagged
`v<v>` the package is `<v>-1`; anything else is a snapshot,
`<v>+git<date>.<sha>-1~ci1`. `<v>` is the version in `package.json`, which
between releases is the last release's, and `+git` sorts after the end of a
version in dpkg: a snapshot sorts above the `<v>-1` release it follows and
below the next one. (`~git` would sort below `<v>-1`.) The snapshot's Debian
revision, `1~ci1`, sorts below `1`.

## Testing in the pipeline

- **During the build** (`debian/rules` `dh_auto_test` override): the full
  Catch2 suite (~everything except `[benchmark]`), compiled and run against
  the *Debian* versions of Catch2, zlib and stb — this is the compatibility
  proof for archive dependency versions.
- **autopkgtest**: `supersonic -v` (superficial) plus the transport harness
  (`test/transport-harness/run.sh`) against the *installed*
  `/usr/bin/supersonic` — boots headless once per transport (UDP, TCP, UDS
  stream, UDS datagram, the shared-memory command plane) and drives OSC load
  over each. The probe client (clockwork's `transport_probe` example) builds
  offline from the vendored crates.
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
- **Rust crates are vendored** — accepted Debian practice for applications,
  but an archive maintainer may prefer `librust-*-dev` packages + `dh-cargo`.
  The dependency surface is small (gilrs, socket2, alsa, plus clockwork's
  in-tree midir fork) and the licence allow-list is machine-enforced by
  `clockwork/rust/deny.toml`.
- **Compiled synthdefs in the source tarball** — the `.scsyndef` files
  under `packages/supersonic-scsynth-synthdefs/` are compiled artifacts whose
  sclang sources live in the Sonic Pi repository (noted in
  `debian/copyright`). They exist only to feed the test suite and are not
  shipped in the .deb. If ftpmaster objects, they can move to
  `Files-Excluded` at the cost of skipping the synthdef-loading tests.
- **`fftlib.c`** — no licence statement (US government work, public domain);
  identical situation to Debian's existing `supercollider` package.
