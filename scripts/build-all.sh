#!/bin/bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2025-2026 Sam Aaron
#
# build-all.sh — every target SuperSonic ships, from one command.
#
#   native   build/native/SuperSonic, the process (and the Catch2 suite)
#   nif      build/nif/clockwork.so, the BEAM NIF
#   web      dist/wasm/scsynth-nrt.wasm and the JavaScript around it
#
# All three are the same CMake tree: the native and NIF builds configure it
# for this machine, the web build configures it under emcmake. The lists of
# sources, the flags and the guest are the tree's; the scripts here only say
# which target and where.
#
#   scripts/build-all.sh             # all three, Release
#   scripts/build-all.sh native web  # some of them
#   scripts/build-all.sh --debug     # Debug throughout
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

DEBUG=""
TARGETS=()
for arg in "$@"; do
    case "$arg" in
        --debug)              DEBUG="--debug" ;;
        native|nif|web)       TARGETS+=("$arg") ;;
        --help|-h)
            sed -n '4,20p' "$0" | sed 's/^# \{0,1\}//'
            exit 0 ;;
        *) echo "unknown argument: $arg" >&2; exit 1 ;;
    esac
done
[ ${#TARGETS[@]} -gt 0 ] || TARGETS=(native nif web)

for t in "${TARGETS[@]}"; do
    echo "═══ $t ═══"
    case "$t" in
        native)
            "$ROOT/scripts/build-native.sh" --tests $DEBUG
            ;;
        nif)
            cmake -B "$ROOT/build/nif" -DCLOCKWORK_NIF=ON \
                -DCMAKE_BUILD_TYPE="$([ -n "$DEBUG" ] && echo Debug || echo Release)" "$ROOT"
            cmake --build "$ROOT/build/nif" --target clockwork_nif --parallel
            ;;
        web)
            "$ROOT/scripts/build-web.sh" $DEBUG
            ;;
    esac
done
