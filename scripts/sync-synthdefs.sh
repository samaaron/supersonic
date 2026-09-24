#!/bin/bash
set -e

# Sonic Pi's synthdefs, as Sonic Pi has them now: its compiled set (etc/synthdefs/compiled) copied into
# packages/supersonic-scsynth-synthdefs, and the package's manifest made again. The package is a copy, and a copy
# nobody refreshes drifts: 0.85.0 went out with the set as it was on 2026-07-10, missing a month of Sonic Pi's fixes
# (the mixer's limiter, the autotuner's silence, krush's bias, hoover, the 808 cymbal, beep's phase_offset).
#
#   scripts/sync-synthdefs.sh <sonic-pi checkout>           copy them in
#   scripts/sync-synthdefs.sh <sonic-pi checkout> --check   say what differs, and fail if anything does
#                                                           (scripts/bump-version.sh runs this before a release)
#
# Two lists say where the package and Sonic Pi part on purpose:
#   LEFT_OUT  Sonic Pi's, not SuperSonic's: winwood_lead reads buffer 0 as a mono buffer of random numbers, which
#             only Sonic Pi's server sets up (removed 2026-01-28)
#   OURS      SuperSonic's own, not Sonic Pi's: mixout, the mixer with an out bus (added 2026-04-10). Kept as it is;
#             it has no Sonic Pi original to follow

LEFT_OUT="sonic-pi-winwood_lead"
OURS="sonic-pi-mixout"

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PACKAGE="$ROOT/packages/supersonic-scsynth-synthdefs/synthdefs"

SONIC_PI="$1"
CHECK=false
[ "$2" = "--check" ] && CHECK=true
if [ -z "$SONIC_PI" ] || [ ! -d "$SONIC_PI/etc/synthdefs/compiled" ]; then
    echo "usage: scripts/sync-synthdefs.sh <sonic-pi checkout> [--check]" >&2
    echo "  (a checkout with etc/synthdefs/compiled; got: ${SONIC_PI:-nothing})" >&2
    exit 2
fi
COMPILED="$SONIC_PI/etc/synthdefs/compiled"

listed() { case " $1 " in *" $2 "*) return 0 ;; esac; return 1; }

changed=0; added=0; removed=0
for f in "$COMPILED"/sonic-pi-*.scsyndef; do
    name="$(basename "$f" .scsyndef)"
    listed "$LEFT_OUT" "$name" && continue
    to="$PACKAGE/$name.scsyndef"
    if [ ! -f "$to" ]; then
        echo "  new:     $name"; added=$((added + 1))
        $CHECK || cp "$f" "$to"
    elif ! cmp -s "$f" "$to"; then
        echo "  changed: $name"; changed=$((changed + 1))
        $CHECK || cp "$f" "$to"
    fi
done
# a synthdef Sonic Pi no longer has (and not one of SuperSonic's own): gone from the package too
for f in "$PACKAGE"/sonic-pi-*.scsyndef; do
    name="$(basename "$f" .scsyndef)"
    listed "$OURS" "$name" && continue
    if [ ! -f "$COMPILED/$name.scsyndef" ]; then
        echo "  gone:    $name"; removed=$((removed + 1))
        $CHECK || rm "$f"
    fi
done

total=$((changed + added + removed))
if $CHECK; then
    if [ "$total" -gt 0 ]; then
        echo "The synthdefs are not Sonic Pi's current ones ($changed changed, $added new, $removed gone): run" >&2
        echo "  scripts/sync-synthdefs.sh $SONIC_PI" >&2
        exit 1
    fi
    echo "The synthdefs are Sonic Pi's current ones."
    exit 0
fi

node "$ROOT/scripts/generate-manifests.mjs"
echo "Synced from $COMPILED: $changed changed, $added new, $removed gone."
