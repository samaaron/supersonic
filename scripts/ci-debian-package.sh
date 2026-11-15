#!/bin/bash

# Build and validate the Debian package the way a Debian maintainer would.
#
# Expects to run *inside* a Debian container (trixie or sid); CI launches one
# per distribution — see .github/workflows/debian.yml. The workflow
# disconnects the container's network before the `build` phase, so the
# package build is a genuine offline-buildability proof; `deps`, `builddeps`,
# `source`, and `autopkgtest` run with network.
#
# Usage:
#   scripts/ci-debian-package.sh <phase>
#
# Phases (in workflow order):
#   deps        packaging toolchain (devscripts, lintian, autopkgtest, …)
#   builddeps   install Build-Depends from packaging/debian/control
#   source      assemble .dsc + orig/component tarballs (make-debian-source.sh)
#   crates      (system crates only) name every Build-Depends the archive
#               cannot supply, and fail if there is one
#   build       extract the .dsc fresh and dpkg-buildpackage it — OFFLINE;
#               runs the full Catch2 suite via debian/rules
#   lintian     lintian on source + binaries, failing on errors AND warnings
#   autopkgtest run debian/tests (transport harness) against the built debs
#   smoke       install the .deb in a FRESH container and boot the server
#               (run this phase in a separate pristine container)

#
# SUPERSONIC_DEB_CRATES=system builds against the archive's own Rust crate
# packages instead of the vendored ones (make-debian-source.sh says how; the
# job is .github/workflows/debian-unstable.yml). The Build-Depends are then
# written by the source phase, so the order is deps, source, crates,
# builddeps, and on as above.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
WORK="$PROJECT_ROOT/build/debian"

export DEBIAN_FRONTEND=noninteractive

setup_env() {
    # CI runs the container as root against a workspace owned by the runner's
    # uid, which git otherwise refuses to touch.
    export GIT_CONFIG_COUNT=1
    export GIT_CONFIG_KEY_0=safe.directory
    export GIT_CONFIG_VALUE_0='*'
}

phase_deps() {
    echo "=== deps: packaging toolchain ==="
    apt-get update
    apt-get install -y --no-install-recommends \
        build-essential devscripts dpkg-dev debhelper equivs \
        lintian autopkgtest \
        git ca-certificates xz-utils
}

# The control file the Build-Depends are read from: the checked-in one, or,
# for a system-crates build, the one the source phase wrote the crate
# packages into.
control_file() {
    if [ "${SUPERSONIC_DEB_CRATES:-vendor}" = system ]; then
        local generated=("$WORK"/supersonic-*/debian/control)
        [ -f "${generated[0]}" ] || { echo "ERROR: run the source phase first" >&2; exit 1; }
        echo "${generated[0]}"
    else
        echo "$PROJECT_ROOT/packaging/debian/control"
    fi
}

phase_builddeps() {
    echo "=== builddeps: install Build-Depends ==="
    mk-build-deps --install --remove \
        --tool 'apt-get -y -o Debug::pkgProblemResolver=yes --no-install-recommends' \
        "$(control_file)"
}

phase_crates() {
    echo "=== crates: which crate packages the archive has ==="
    apt-get update -qq
    local wanted missing=() pkg
    wanted="$(sed -n 's/^ \(librust-[a-z0-9+.-]*-dev\),\{0,1\}$/\1/p' "$(control_file)")"
    [ -n "$wanted" ] || { echo "ERROR: no librust-*-dev in Build-Depends: nothing was checked" >&2; exit 1; }
    for pkg in $wanted; do
        # A real package or one another provides: either installs.
        if apt-get install -s --no-install-recommends "$pkg" >/dev/null 2>&1; then
            echo "  have     $pkg"
        else
            echo "  MISSING  $pkg"
            missing+=("$pkg")
        fi
    done
    echo "$(wc -w <<<"$wanted") crate packages checked, ${#missing[@]} missing"
    if [ "${#missing[@]}" -gt 0 ]; then
        echo "FAIL: the archive cannot supply: ${missing[*]}" >&2
        exit 1
    fi
}

phase_source() {
    echo "=== source package ==="
    setup_env
    "$SCRIPT_DIR/make-debian-source.sh" "$WORK"
}

phase_build() {
    echo "=== build (offline) ==="
    # Loudly record whether the offline claim is actually being tested.
    if timeout 5 bash -c 'exec 3<>/dev/tcp/deb.debian.org/80' 2>/dev/null; then
        echo "WARNING: network is reachable — this build does NOT prove offline buildability" >&2
    else
        echo "network unreachable — offline build proof is live"
    fi
    rm -rf "$WORK/build-area"
    mkdir -p "$WORK/build-area"
    dsc=("$WORK"/*.dsc)
    # Extract the freshly built source package rather than reusing the
    # assembly tree: this is what proves the .dsc is complete.
    (cd "$WORK/build-area" && dpkg-source -x "${dsc[0]}" src)
    (cd "$WORK/build-area/src" && dpkg-buildpackage -us -uc -b)
}

phase_lintian() {
    echo "=== lintian (fail on errors + warnings) ==="
    lintian --fail-on error,warning --info "$WORK"/*.dsc
    lintian --fail-on error,warning --info "$WORK"/build-area/*.changes
}

phase_autopkgtest() {
    echo "=== autopkgtest ==="
    # debs + .dsc (a binary-only .changes does not reference the source, and
    # autopkgtest needs the source tree for debian/tests).
    autopkgtest "$WORK"/build-area/*.deb "$WORK"/*.dsc -- null
}

phase_smoke() {
    echo "=== smoke: install + boot in a pristine container ==="
    deb=("$WORK"/build-area/supersonic_*_"$(dpkg --print-architecture)".deb)
    # Inspect the deb's contents (the official Debian container images
    # path-exclude /usr/share/man, so the man page never hits the filesystem on
    # install — check the archive itself). Capture the listing once and match
    # via here-strings: `dpkg-deb -c | grep -q` would SIGPIPE dpkg-deb's tar
    # when grep short-circuits, and `set -o pipefail` turns that into a failure.
    contents="$(dpkg-deb -c "${deb[0]}")"
    grep -q '/usr/bin/supersonic$' <<<"$contents" \
        || { echo "FAIL: deb has no /usr/bin/supersonic" >&2; exit 1; }
    grep -q '/usr/share/man/man1/supersonic\.1\.gz$' <<<"$contents" \
        || { echo "FAIL: deb has no man page" >&2; exit 1; }
    apt-get update
    apt-get install -y "${deb[0]}"
    supersonic -v
    # Boot as installed, headless (no audio device in the container), with a
    # secret on stdin and a TCP command port, give it a moment, then require
    # it to still be alive. Replies over each transport are autopkgtest's
    # (transport-smoke).
    secret="$(head -c 32 /dev/urandom | od -An -tx1 | tr -d ' \n')"
    printf '%s\n' "$secret" | supersonic --headless --tcp 57110 > /tmp/supersonic-smoke.log 2>&1 &
    pid=$!
    sleep 5
    if ! kill -0 "$pid" 2>/dev/null; then
        echo "FAIL: server exited during smoke boot — log:" >&2
        sed 's/^/  | /' /tmp/supersonic-smoke.log >&2
        exit 1
    fi
    kill "$pid"; wait "$pid" 2>/dev/null || true
    echo "smoke OK"
}

case "${1:-}" in
    deps)        phase_deps ;;
    builddeps)   phase_builddeps ;;
    source)      phase_source ;;
    crates)      phase_crates ;;
    build)       phase_build ;;
    lintian)     phase_lintian ;;
    autopkgtest) phase_autopkgtest ;;
    smoke)       phase_smoke ;;
    *)
        echo "Usage: $0 {deps|builddeps|source|crates|build|lintian|autopkgtest|smoke}" >&2
        exit 1
        ;;
esac
