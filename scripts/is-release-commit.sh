#!/bin/bash

# Is HEAD the release commit: the one that set package.json to its version,
# which scripts/bump-version.sh makes and tags?
#
# Exit 0 if it is, 1 if it is not, 2 if that cannot be told.
#
# HEAD is the version commit when its own change to package.json is the line
# that names the version. A clone cut off at HEAD shows that whole file as
# added, so the parent must be there (fetch-depth 2 in CI) for the answer to
# mean anything.
#
# Used by scripts/make-debian-source.sh, to tell a release package from a
# snapshot, and by .github/workflows/debian-unstable.yml, which builds
# releases only.

set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."

V="$(sed -n 's/^  "version": "\([0-9][0-9.]*\)",$/\1/p' package.json | head -1)"
[ -n "$V" ] || { echo "ERROR: no version in package.json" >&2; exit 2; }
git rev-parse --verify -q HEAD^ >/dev/null || {
    echo "ERROR: HEAD has no parent in this clone; fetch at least depth 2 to tell a release from a snapshot" >&2
    exit 2
}
git show --format= HEAD -- package.json | grep -xF "+  \"version\": \"$V\"," >/dev/null
