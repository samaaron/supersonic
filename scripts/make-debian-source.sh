#!/bin/bash

# Assemble a real Debian source package (.dsc + orig + component tarballs)
# for SuperSonic, the way a Debian maintainer would.
#
# Layout produced (in build/debian/):
#   supersonic_<uv>.orig.tar.xz             git archive of HEAD with the
#                                           clockwork submodule archived into
#                                           clockwork/, minus the Files-Excluded
#                                           set in packaging/debian/copyright
#                                           (the ASIO SDK, and clockwork's copy
#                                           of libsamplerate: the package links
#                                           the archive's)
#   supersonic_<uv>.orig-link.tar.xz        pristine Ableton Link 4.0 with its
#                                           asio-standalone submodule (the
#                                           GitHub tag tarball lacks
#                                           submodules, hence the clone)
#   supersonic_<uv>.orig-rust-vendor.tar.xz `cargo vendor` output for the
#                                           committed clockwork/rust/Cargo.lock
#   supersonic_<dv>.dsc + .debian.tar.xz    via dpkg-buildpackage -S
#
# The four Link patches are clockwork's (clockwork/external/*.patch); they are
# path-shifted under link/ into debian/patches here so dpkg-source applies
# them to the component tree.
#
# Versioning: the release build is the commit that set package.json to
# <version>, the one scripts/bump-version.sh makes and tags; it gets
# <version>-1. Every other commit gets <version>+git<date>.<sha>-1~ci1 so
# snapshot packages sort below the eventual release. CI builds the package
# when the commit reaches main, before the tag exists, so the tag cannot be
# what tells a release from a snapshot.
#
# Needs network (Link clone + crates.io) — run it in the *networked* phase;
# the package build itself then proves it needs none. Archives HEAD, so
# uncommitted changes are not included.
#
# Two ways to supply the Rust crates, chosen by SUPERSONIC_DEB_CRATES:
#
#   vendor (default)  the orig-rust-vendor component above: every crate in
#                     the lock file, carried in the source package.
#   system            no vendor component. The build takes the archive's own
#                     crate packages (librust-*-dev), which is the form the
#                     Debian archive accepts. clockwork's manifests are
#                     reduced to Linux by a generated patch
#                     (clockwork/scripts/linux-manifests.py: cargo resolves
#                     every platform's crates otherwise), the lock file goes
#                     with it, and Build-Depends gains one librust-<crate>-dev
#                     for each crate clockwork depends on directly.
#
# SUPERSONIC_DEB_GAMEPAD=off (system only) also leaves game controller input
# out: clockwork-gamepad leaves the workspace and the build is configured
# with -DCLOCKWORK_GAMEPAD=OFF. It exists to build everything else while the
# archive has no gilrs.
#
# Usage: scripts/make-debian-source.sh [workdir]   (default: build/debian)

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
WORK="${1:-$PROJECT_ROOT/build/debian}"

CRATES="${SUPERSONIC_DEB_CRATES:-vendor}"
GAMEPAD="${SUPERSONIC_DEB_GAMEPAD:-on}"
case "$CRATES" in vendor|system) ;; *) echo "ERROR: SUPERSONIC_DEB_CRATES must be vendor or system" >&2; exit 1 ;; esac
case "$GAMEPAD" in on|off) ;; *) echo "ERROR: SUPERSONIC_DEB_GAMEPAD must be on or off" >&2; exit 1 ;; esac
if [ "$GAMEPAD" = off ] && [ "$CRATES" != system ]; then
    echo "ERROR: SUPERSONIC_DEB_GAMEPAD=off needs SUPERSONIC_DEB_CRATES=system" >&2; exit 1
fi

LINK_TAG="Link-4.1"
LINK_REPO="https://github.com/Ableton/link.git"

cd "$PROJECT_ROOT"

# ── Version ──────────────────────────────────────────────────────────────────
# package.json is the one place the version is written by hand;
# scripts/bump-version.sh carries it to CMakeLists.txt and the packages.
V="$(sed -n 's/^  "version": "\([0-9][0-9.]*\)",$/\1/p' package.json | head -1)"
[ -n "$V" ] || { echo "ERROR: no version in package.json" >&2; exit 1; }
SHA="$(git rev-parse --short HEAD)"

# The release commit (scripts/is-release-commit.sh) or a snapshot of any other.
release=0
"$SCRIPT_DIR/is-release-commit.sh" || release=$?
case "$release" in
    0)  UV="$V"
        DEBV="$V-1"
        SNAPSHOT="" ;;
    1)  UV="$V+git$(date -u +%Y%m%d).$SHA"
        DEBV="$UV-1~ci1"
        SNAPSHOT="yes" ;;
    *)  exit 1 ;;
esac
echo "=== supersonic $DEBV (upstream $UV) ==="

# Refuse to wipe a pre-existing directory this script didn't create (the
# sentinel guards against e.g. an accidental `make-debian-source.sh ~`).
if [ -e "$WORK" ] && [ ! -e "$WORK/.supersonic-debian-work" ]; then
    echo "ERROR: $WORK exists but was not created by this script; refusing to delete it." >&2
    exit 1
fi
rm -rf "$WORK"
mkdir -p "$WORK"
touch "$WORK/.supersonic-debian-work"
SRC="$WORK/supersonic-$UV"

# ── Main orig tarball: git archive minus Files-Excluded ─────────────────────
echo "=== orig tarball ==="
git archive --format=tar --prefix="supersonic-$UV/" HEAD | tar -x -C "$WORK"
# git archive stops at the submodule boundary: clockwork/ comes out as an
# empty directory. Archive the submodule's own HEAD into it, so the orig
# tarball is the whole program.
git -C clockwork archive --format=tar --prefix="supersonic-$UV/clockwork/" HEAD | tar -x -C "$WORK"
# Keep in sync with Files-Excluded in packaging/debian/copyright.
rm -rf "$SRC/clockwork/external/ASIOSDK2.3.4"
rm -rf "$SRC/clockwork/src/vendor/libsamplerate"
tar -C "$WORK" -cJf "$WORK/supersonic_$UV.orig.tar.xz" "supersonic-$UV"

# ── Link component: pristine upstream incl. submodules ──────────────────────
echo "=== orig-link tarball ($LINK_TAG) ==="
git clone --quiet --depth 1 --branch "$LINK_TAG" \
    --recurse-submodules --shallow-submodules "$LINK_REPO" "$WORK/link"
find "$WORK/link" -name .git -prune -exec rm -rf {} +
tar -C "$WORK" -cJf "$WORK/supersonic_$UV.orig-link.tar.xz" link

# ── Rust vendor component ───────────────────────────────────────────────────
if [ "$CRATES" = vendor ]; then
echo "=== orig-rust-vendor tarball ==="
(cd clockwork/rust && cargo vendor "$WORK/rust-vendor")

# Sanitise the vendor tree so it survives Debian's source-package machinery and
# lintian. For each affected crate we delete the offending files AND strip their
# entries from .cargo-checksum.json, so cargo (which only checks listed files,
# and only for crates it actually builds) stays consistent. Two classes:
#
#   *.orig — newer cargo writes Cargo.toml.orig for crates whose manifest it
#     rewrites; dpkg-source's default tar-ignore drops *.orig as patch backups,
#     so cargo would abort the offline build on a checksummed-but-missing file.
#
#   Prebuilt Windows binaries — the winapi *-pc-windows-gnu crates ship prebuilt
#     import libraries (lib/*.a) and libloading ships Windows test DLLs. None are
#     compiled on a Linux target, but they trip lintian (ar-unreadable .a =
#     error; *.dll = source-contains-prebuilt-windows-binary warning) and would
#     be a DFSG smell in the source package. The Windows build uses crates.io
#     directly, so pruning them from the Linux vendor tarball costs nothing.
python3 - "$WORK/rust-vendor" <<'PY'
import json, os, sys
root = sys.argv[1]

def is_junk(crate, key):
    if key.endswith(".orig"):
        return True
    if key.endswith((".dll", ".dll.a")):
        return True
    # Prebuilt import/static libs only inside the Windows target crates, so a
    # legitimately source-shipped Linux .a (none known) is never touched.
    if "windows" in crate and key.endswith((".a", ".lib")):
        return True
    return False

for crate in sorted(os.listdir(root)):
    cksum = os.path.join(root, crate, ".cargo-checksum.json")
    if not os.path.isfile(cksum):
        continue
    with open(cksum) as f:
        data = json.load(f)
    files = data.get("files", {})
    removed = [k for k in files if is_junk(crate, k)]
    for k in removed:
        del files[k]
        p = os.path.join(root, crate, k)
        if os.path.exists(p):
            os.remove(p)
    if removed:
        with open(cksum, "w") as f:
            json.dump(data, f, separators=(",", ":"))
        print(f"  sanitised {crate}: dropped {len(removed)} file(s)")
PY
tar -C "$WORK" -cJf "$WORK/supersonic_$UV.orig-rust-vendor.tar.xz" rust-vendor
fi

# ── Assemble the source tree ────────────────────────────────────────────────
echo "=== source tree ==="
cp -a "$WORK/link" "$SRC/link"
[ "$CRATES" = vendor ] && cp -a "$WORK/rust-vendor" "$SRC/rust-vendor"
cp -a packaging/debian "$SRC/debian"
# What debian/rules builds from: read there as debian/build-mode.mk.
printf 'CRATES = %s\nGAMEPAD = %s\n' "$CRATES" "$GAMEPAD" > "$SRC/debian/build-mode.mk"

# Path-shift the Link patches under the link/ component.
mkdir -p "$SRC/debian/patches"
: > "$SRC/debian/patches/series"
for p in clockwork/external/link-*.patch; do
    name="$(basename "$p")"
    sed -e 's|^--- a/|--- a/link/|' -e 's|^+++ b/|+++ b/link/|' \
        "$p" > "$SRC/debian/patches/$name"
    echo "$name" >> "$SRC/debian/patches/series"
done

# ── The archive's crates: Linux-only manifests, no lock file ────────────────
if [ "$CRATES" = system ]; then
    echo "=== Linux-only manifests (patch) and Build-Depends ==="
    MF="$WORK/manifests"
    mkdir -p "$MF/a/clockwork" "$MF/b/clockwork"
    for side in a b; do
        cp -a "$SRC/clockwork/rust" "$SRC/clockwork/external" "$MF/$side/clockwork/"
    done
    drop=(--drop-member clockwork-guards)
    [ "$GAMEPAD" = off ] && drop+=(--drop-member clockwork-gamepad)
    python3 "$SRC/clockwork/scripts/linux-manifests.py" "${drop[@]}" --list-crates \
        "$MF/b/clockwork" > "$MF/crates"
    # The archive's versions are not the lock file's: cargo resolves against
    # what is installed (debian/rules: -DCLOCKWORK_CARGO_LOCKED=OFF).
    rm "$MF/b/clockwork/rust/Cargo.lock"
    # diff exits 1 when the trees differ, which is the only acceptable answer.
    rc=0
    (cd "$MF" && diff -ruN a b > linux-only-manifests.patch) || rc=$?
    [ "$rc" = 1 ] || { echo "ERROR: diff of the rewritten manifests exited $rc, expected 1 (they differ)" >&2; exit 1; }
    cp "$MF/linux-only-manifests.patch" "$SRC/debian/patches/"
    echo linux-only-manifests.patch >> "$SRC/debian/patches/series"

    # One librust-<crate>-dev per crate clockwork names; apt brings the rest.
    [ -s "$MF/crates" ] || { echo "ERROR: linux-manifests.py listed no crates" >&2; exit 1; }
    deps="$(sed -e 's/_/-/g' -e 's/.*/ librust-&-dev,/' "$MF/crates")"
    python3 - "$SRC/debian/control" "$deps" <<'PY'
import sys
path, deps = sys.argv[1], sys.argv[2]
text = open(path).read()
anchor = " rustc,\n"
assert text.count(anchor) == 1, "debian/control: expected one ' rustc,' line in Build-Depends"
open(path, "w").write(text.replace(anchor, anchor + deps + "\n"))
PY
    # The installed-package test builds its probe from the same crates.
    python3 - "$SRC/debian/tests/control" <<'PY'
import sys
path = sys.argv[1]
text = open(path).read()
anchor = "Depends: supersonic, cargo, rustc\n"
assert text.count(anchor) == 1, "debian/tests/control: expected the transport-smoke Depends line"
open(path, "w").write(text.replace(anchor, "Depends: supersonic, cargo, rustc, @builddeps@\n"))
PY
    # No vendored crates in this source package: its copyright stanza would
    # match nothing.
    python3 - "$SRC/debian/copyright" <<'PY'
import re, sys
path = sys.argv[1]
text = open(path).read()
stanzas = text.split("\n\n")
kept = [s for s in stanzas if not s.startswith("Files: rust-vendor/*")]
assert len(kept) == len(stanzas) - 1, "debian/copyright: expected one 'Files: rust-vendor/*' stanza"
open(path, "w").write("\n\n".join(kept))
PY
    echo "  $(wc -l < "$MF/crates") crate packages added to Build-Depends; gamepad $GAMEPAD"
fi

# Reconcile the changelog with the computed version. Snapshots always need a
# new entry; releases need one too whenever the checked-in changelog hasn't
# caught up with a version bump (otherwise dpkg-buildpackage would look for an
# orig tarball named after the stale changelog version and fail).
CHANGELOG_V="$(dpkg-parsechangelog -l "$SRC/debian/changelog" -S Version)"
if [ "$CHANGELOG_V" != "$DEBV" ]; then
    if [ -n "$SNAPSHOT" ]; then
        entry="CI snapshot of git $SHA."
    else
        entry="New upstream release $V (auto-generated entry; see git history)."
    fi
    (cd "$SRC" && DEBEMAIL="sam@sonic-pi.net" DEBFULLNAME="Sam Aaron" \
        dch --newversion "$DEBV" --distribution unstable --force-distribution \
            "$entry")
fi

# ── Build the source package ────────────────────────────────────────────────
echo "=== dpkg-buildpackage -S ==="
(cd "$SRC" && dpkg-buildpackage -S -us -uc -d)

echo "=== done ==="
ls -l "$WORK"/*.dsc "$WORK"/*.tar.xz
