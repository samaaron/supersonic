#!/bin/bash
set -euo pipefail

# Publishes the npm package in the current directory, unless npm already has
# this version of it. A release's packages go out one step at a time, and npm
# never takes a version twice: with this, re-running a publish that stopped
# part way publishes the rest.
#
#   scripts/npm-publish-once.sh [npm publish options]   e.g. --dry-run

name=$(node -p "require('./package.json').name")
version=$(node -p "require('./package.json').version")
published=$(npm view "$name@$version" version 2>/dev/null || true)
if [ "$published" = "$version" ]; then
    echo "$name@$version is already on npm: not published again"
    exit 0
fi
npm publish "$@"
