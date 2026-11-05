#!/usr/bin/env bash
# scsynth is a guest any number of which can run in one process (clockwork's
# DspInfo::instances = DSP_INSTANCES_ANY): a guest that keeps nothing
# process-wide, so no mutable variable with static storage anywhere in its
# code. Clockwork's check (clockwork/scripts/check-guest-globals.sh) allows
# none at all; this counts what scsynth has against the budget in
# scripts/guest-globals.budget, which is 0, and fails when the count rises
# (or falls below a budget that was not lowered with it).
#
# Reads the objects of an unoptimised build: an optimised one merges file
# statics into unnamed blocks.
#
#   scripts/check-guest-globals.sh [build-dir]   (default build/native-debug)
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
build="${1:-$root/build/native-debug}"
budget_file="$root/scripts/guest-globals.budget"
check="$root/clockwork/scripts/check-guest-globals.sh"
objects_dir="$build/clockwork/dsp-scsynth"
if [ ! -d "$objects_dir" ]; then
    echo "check-guest-globals: no scsynth objects under $objects_dir; build scsynth with -DCMAKE_BUILD_TYPE=Debug" >&2
    exit 2
fi
budget=$(tr -d '[:space:]' < "$budget_file")
report=$(mktemp)
trap 'rm -f "$report"' EXIT
objects=0
while IFS= read -r object; do
    objects=$((objects + 1))
    # One object at a time: each line is "<file>: <name> (<section>): <why>".
    "$check" "$object" 2>/dev/null | grep '^  ' | sed 's/: a guest that says.*//' >> "$report" || true
done < <(find "$objects_dir" -name '*.o' | sort)
if [ "$objects" -eq 0 ]; then
    echo "check-guest-globals: found no objects under $objects_dir" >&2
    exit 2
fi
count=$(wc -l < "$report" | tr -d ' ')
echo "check-guest-globals: read $objects scsynth objects; $count mutable statics, budget $budget"
if [ "$count" -gt "$budget" ]; then
    echo "  above the budget: a guest that will say DSP_INSTANCES_ANY keeps nothing process-wide" >&2
    sort "$report" >&2
    exit 1
fi
if [ "$count" -lt "$budget" ]; then
    echo "  below the budget: lower scripts/guest-globals.budget to $count so this slice is recorded" >&2
    exit 1
fi
