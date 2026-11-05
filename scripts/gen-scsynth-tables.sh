#!/usr/bin/env bash
# Regenerate dsp/scsynth/scsynth_tables.cpp, scsynth's lookup tables as
# constants (scripts/gen-scsynth-tables.cpp says which and how). Run after a
# change to a table's formula or size; test/native/test_scsynth_tables.cpp
# fails until the data matches the formulas again.
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
c++ -std=c++17 -O1 -o "$work/gen" "$root/scripts/gen-scsynth-tables.cpp"
"$work/gen" "$root/dsp/scsynth/scsynth_tables.cpp"
echo "gen-scsynth-tables: wrote dsp/scsynth/scsynth_tables.cpp ($(wc -c < "$root/dsp/scsynth/scsynth_tables.cpp" | tr -d ' ') bytes)"
