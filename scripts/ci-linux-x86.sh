#!/bin/bash

# Build and test SuperSonic for 32-bit x86 (i686).
#
# Expects to already be running *inside* a 32-bit Debian userland — CI launches
# that container; see the native-linux-x86 job in .github/workflows/native.yml.
#
# Why a container rather than multiarch on the normal runner: Ubuntu dropped
# i386 as a full architecture in 19.10, so most of the -dev packages JUCE needs
# (X11, freetype, fontconfig, ALSA, JACK) have no i386 build there. Debian
# still carries a complete i386 archive. Nothing is emulated — x86_64 CPUs run
# 32-bit code natively, so this costs about what the x64 job does.
#
# 32-bit is worth covering because it is the one desktop target where the
# engine's lock-free assumptions can actually break: on i686, 8-byte atomics
# lower to cmpxchg8b and may need libatomic, where x86_64 gets them for free.
# The bit-packed atomic<uint64_t> seen in TimeSource.h / LinkAudioInputRenderer
# exists for exactly this reason, and this job is what proves it holds.
#
# Usage:
#   scripts/ci-linux-x86.sh [phase]
#
# Phases (default `all` runs them in order):
#   deps       apt dependencies + Rust toolchain
#   configure  cmake configure with tests on
#   build      SuperSonic binary + native test suite
#   test       Catch2 suite (benchmarks excluded, as in the x64 job)
#   transport  end-to-end transport harness

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="$PROJECT_ROOT/build/native-x86"

check_arch() {
    if [ "$(getconf LONG_BIT)" != "32" ]; then
        echo "ERROR: this script must run inside a 32-bit userland (getconf LONG_BIT = $(getconf LONG_BIT))." >&2
        exit 1
    fi
}

setup_paths() {
    if [ -f "${CARGO_HOME:-$HOME/.cargo}/env" ]; then
        # shellcheck disable=SC1091
        . "${CARGO_HOME:-$HOME/.cargo}/env"
    fi
    # CI runs the container as root against a workspace owned by the runner's
    # uid, which git otherwise refuses to touch. Scope it to the environment
    # rather than rewriting a global gitconfig.
    export GIT_CONFIG_COUNT=1
    export GIT_CONFIG_KEY_0=safe.directory
    export GIT_CONFIG_VALUE_0='*'
}

phase_deps() {
    echo "=== deps: apt + Rust ==="
    export DEBIAN_FRONTEND=noninteractive
    apt-get update
    # Mirrors the x64 job's dependency list. libjack-jackd2-dev rather than the
    # virtual libjack-dev: Debian has several providers, so apt can't resolve
    # the virtual name on its own.
    apt-get install -y \
        build-essential cmake pkg-config \
        ca-certificates curl git \
        libasound2-dev libudev-dev libjack-jackd2-dev \
        libfreetype-dev libfontconfig1-dev libx11-dev libxrandr-dev \
        libxinerama-dev libxcursor-dev libxcomposite-dev

    # The Rust subsystems (MIDI/gamepad/OSC) are cargo-built staticlibs the
    # native build links. rustup on an i386 userland resolves the host triple
    # to i686-unknown-linux-gnu, a tier-1 target.
    if ! command -v cargo >/dev/null 2>&1 && [ ! -x "${CARGO_HOME:-$HOME/.cargo}/bin/cargo" ]; then
        curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs \
            | sh -s -- -y --default-toolchain stable --profile minimal
    fi
}

phase_configure() {
    echo "=== configure ==="
    setup_paths
    cmake -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTS=ON
}

phase_build() {
    echo "=== build: SuperSonic + tests ==="
    setup_paths
    cmake --build "$BUILD_DIR" --config Release --parallel --target SuperSonic
    # Capped at 2 parallel jobs to match the x64 job: the test target recompiles
    # JUCE alongside Catch2 and the test sources, and unbounded parallelism can
    # exhaust memory on heavy modules like juce_audio_devices.
    cmake --build "$BUILD_DIR" --config Release --parallel 2 --target SuperSonicNativeTests
}

phase_test() {
    echo "=== test: Catch2 suite ==="
    setup_paths
    SUPERSONIC_QUIET=1 "$BUILD_DIR/test/native/SuperSonicNativeTests" "~[benchmark]"
}

phase_transport() {
    echo "=== transport harness ==="
    setup_paths
    # run.sh defaults to build/native/...; pass the binary explicitly since this
    # job builds into a separate dir so it can't clash with a local x64 build.
    "$PROJECT_ROOT/test/transport-harness/run.sh" \
        "$BUILD_DIR/SuperSonic_artefacts/Release/SuperSonic"
}

main() {
    check_arch
    case "${1:-all}" in
        deps)      phase_deps ;;
        configure) phase_configure ;;
        build)     phase_build ;;
        test)      phase_test ;;
        transport) phase_transport ;;
        all)
            phase_deps
            phase_configure
            phase_build
            phase_test
            phase_transport
            ;;
        --help|-h)
            sed -n '3,30p' "${BASH_SOURCE[0]}"
            ;;
        *)
            echo "Unknown phase: $1" >&2
            echo "Valid: all deps configure build test transport" >&2
            exit 1
            ;;
    esac
}

main "$@"
