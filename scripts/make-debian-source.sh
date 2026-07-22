#!/bin/bash

# Assemble a real Debian source package (.dsc + orig + component tarballs)
# for SuperSonic, the way a Debian maintainer would.
#
# Layout produced (in build/debian/):
#   supersonic_<uv>.orig.tar.xz             git archive of HEAD, minus the
#                                           Files-Excluded set in
#                                           packaging/debian/copyright
#   supersonic_<uv>.orig-link.tar.xz        pristine Ableton Link 4.0 with its
#                                           asio-standalone submodule (the
#                                           GitHub tag tarball lacks
#                                           submodules, hence the clone)
#   supersonic_<uv>.orig-rust-vendor.tar.xz `cargo vendor` output for the
#                                           committed rust/Cargo.lock
#   supersonic_<dv>.dsc + .debian.tar.xz    via dpkg-buildpackage -S
#
# The four Link patches stay single-sourced in external/*.patch; they are
# path-shifted under link/ into debian/patches here so dpkg-source applies
# them to the component tree.
#
# Versioning: releases (HEAD == tag v<version>) get <version>-1; anything else
# gets <version>+git<date>.<sha>-1~ci1 so snapshot packages sort below the
# eventual release.
#
# Needs network (Link clone + crates.io) — run it in the *networked* phase;
# the package build itself then proves it needs none. Archives HEAD, so
# uncommitted changes are not included.
#
# Usage: scripts/make-debian-source.sh [workdir]   (default: build/debian)

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
WORK="${1:-$PROJECT_ROOT/build/debian}"

LINK_TAG="Link-4.0"
LINK_REPO="https://github.com/Ableton/link.git"

cd "$PROJECT_ROOT"

# ── Version ──────────────────────────────────────────────────────────────────
ver_component() {
    sed -n "s/^#define SUPERSONIC_VERSION_$1 \([0-9]*\)$/\1/p" src/supersonic_config.h
}
V="$(ver_component MAJOR).$(ver_component MINOR).$(ver_component PATCH)"
SHA="$(git rev-parse --short HEAD)"

if git describe --tags --exact-match HEAD 2>/dev/null | grep -qx "v$V"; then
    UV="$V"
    DEBV="$V-1"
    SNAPSHOT=""
else
    UV="$V+git$(date -u +%Y%m%d).$SHA"
    DEBV="$UV-1~ci1"
    SNAPSHOT="yes"
fi
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
# Keep in sync with Files-Excluded in packaging/debian/copyright.
rm -rf "$SRC/external_libs/ASIOSDK2.3.4"
tar -C "$WORK" -cJf "$WORK/supersonic_$UV.orig.tar.xz" "supersonic-$UV"

# ── Link component: pristine upstream incl. submodules ──────────────────────
echo "=== orig-link tarball ($LINK_TAG) ==="
git clone --quiet --depth 1 --branch "$LINK_TAG" \
    --recurse-submodules --shallow-submodules "$LINK_REPO" "$WORK/link"
find "$WORK/link" -name .git -prune -exec rm -rf {} +
tar -C "$WORK" -cJf "$WORK/supersonic_$UV.orig-link.tar.xz" link

# ── Rust vendor component ───────────────────────────────────────────────────
echo "=== orig-rust-vendor tarball ==="
(cd rust && cargo vendor "$WORK/rust-vendor")
tar -C "$WORK" -cJf "$WORK/supersonic_$UV.orig-rust-vendor.tar.xz" rust-vendor

# ── Assemble the source tree ────────────────────────────────────────────────
echo "=== source tree ==="
cp -a "$WORK/link" "$SRC/link"
cp -a "$WORK/rust-vendor" "$SRC/rust-vendor"
cp -a packaging/debian "$SRC/debian"

# Path-shift the Link patches under the link/ component.
mkdir -p "$SRC/debian/patches"
: > "$SRC/debian/patches/series"
for p in external/link-*.patch; do
    name="$(basename "$p")"
    sed -e 's|^--- a/|--- a/link/|' -e 's|^+++ b/|+++ b/link/|' \
        "$p" > "$SRC/debian/patches/$name"
    echo "$name" >> "$SRC/debian/patches/series"
done

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
