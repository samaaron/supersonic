#!/bin/bash
set -e

# SuperSonic Version Bump Script
#
# This script updates version numbers in exactly 13 locations:
#
# Package.json version fields (5):
#   1. package.json
#   2. packages/supersonic-scsynth-core/package.json
#   3. packages/supersonic-scsynth-synthdefs/package.json
#   4. packages/supersonic-scsynth-samples/package.json
#   5. packages/supersonic-scsynth-bundle/package.json
#
# Bundle dependencies (4):
#   6. packages/supersonic-scsynth-bundle/package.json - supersonic-scsynth dependency
#   7. packages/supersonic-scsynth-bundle/package.json - supersonic-scsynth-synthdefs dependency
#   8. packages/supersonic-scsynth-bundle/package.json - supersonic-scsynth-samples dependency
#   8b. packages/supersonic-scsynth-bundle/package.json - supersonic-scsynth-core dependency
#
# CDN constants in index.js (3):
#   8. packages/supersonic-scsynth-synthdefs/index.js - CDN_BASE constant
#   9. packages/supersonic-scsynth-samples/index.js - UNPKG_BASE constant
#  10. packages/supersonic-scsynth-samples/index.js - JSDELIVR_BASE constant
#
# Documentation version examples (1):
#  11. docs/INSTALLATION_WEB.md - pinned version example
#
# And what it used to miss:
#  13. package-lock.json (through npm)
#  14. example/*.html - the versions their CDN URLs pin
#  15. packaging/debian/changelog - a new entry
#
# A version in prose about an older release (js/supersonic.js, the specs) is history and is left alone.
#
# C++ version constants (1):
#  12. CMakeLists.txt - project(SuperSonic VERSION x.y.z), which clockwork
#      turns into the banner, the -v output and the Windows version resource
#
# Note: READMEs and error messages use @latest and don't need version updates

# Color output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

# Check if version argument provided
if [ -z "$1" ]; then
    echo -e "${RED}Error: Version number required${NC}"
    echo "Usage: scripts/bump-version.sh <version>"
    echo "Example: scripts/bump-version.sh 0.1.2"
    exit 1
fi

NEW_VERSION=$1
shift || true
PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

# --yes: no prompt. --no-build: leave dist to `npm publish` (its prepublishOnly builds --release).
# --no-commit: edit and stage only, for a repository whose commits are made by hand.
ASSUME_YES=false; DO_BUILD=true; DO_COMMIT=true
for arg in "$@"; do
    case "$arg" in
        --yes|-y)    ASSUME_YES=true ;;
        --no-build)  DO_BUILD=false ;;
        --no-commit) DO_COMMIT=false ;;
        *) echo "unknown argument: $arg" >&2; exit 1 ;;
    esac
done

# in place, on BSD sed (macOS) as on GNU's: -i takes a suffix there, so one is given and the backup removed
sedi() { local script=$1; shift; sed -i.versionbak "$script" "$@" && rm -f "${@/%/.versionbak}"; }

# Validate version format (basic check)
if ! [[ "$NEW_VERSION" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
    echo -e "${RED}Error: Invalid version format${NC}"
    echo "Version must be in format: X.Y.Z (e.g., 0.1.2)"
    exit 1
fi

echo "========================================"
echo "  SuperSonic Version Bump"
echo "========================================"
echo ""
echo -e "${YELLOW}New version: $NEW_VERSION${NC}"
echo ""

# Get current version for reference
CURRENT_VERSION=$(node -p "require('./package.json').version")
echo "Current version: $CURRENT_VERSION"
echo ""

# Confirm
if [ "$ASSUME_YES" = false ]; then read -p "Continue with version bump? (y/n) " -n 1 -r; echo; else REPLY=y; fi
if [[ ! $REPLY =~ ^[Yy]$ ]]; then
    echo "Aborted."
    exit 0
fi

echo ""
echo "Step 1: Updating package.json files..."
echo "---------------------------------------"

# Update 4 package.json version fields
sedi "s/\"version\": \"$CURRENT_VERSION\"/\"version\": \"$NEW_VERSION\"/" "$PROJECT_ROOT/package.json"
echo "✓ Updated package.json"

sedi "s/\"version\": \".*\"/\"version\": \"$NEW_VERSION\"/" "$PROJECT_ROOT/packages/supersonic-scsynth-core/package.json"
echo "✓ Updated packages/supersonic-scsynth-core/package.json"

sedi "s/\"version\": \"$CURRENT_VERSION\"/\"version\": \"$NEW_VERSION\"/" "$PROJECT_ROOT/packages/supersonic-scsynth-synthdefs/package.json"
echo "✓ Updated packages/supersonic-scsynth-synthdefs/package.json"

sedi "s/\"version\": \"$CURRENT_VERSION\"/\"version\": \"$NEW_VERSION\"/" "$PROJECT_ROOT/packages/supersonic-scsynth-samples/package.json"
echo "✓ Updated packages/supersonic-scsynth-samples/package.json"

sedi "s/\"version\": \"$CURRENT_VERSION\"/\"version\": \"$NEW_VERSION\"/" "$PROJECT_ROOT/packages/supersonic-scsynth-bundle/package.json"
echo "✓ Updated packages/supersonic-scsynth-bundle/package.json"

echo ""
echo "Step 2: Updating bundle dependencies..."
echo "----------------------------------------"

# Update 3 bundle dependencies (use caret dependencies)
sedi "s/\"supersonic-scsynth\": \"\\^$CURRENT_VERSION\"/\"supersonic-scsynth\": \"^$NEW_VERSION\"/" "$PROJECT_ROOT/packages/supersonic-scsynth-bundle/package.json"
echo "✓ Updated supersonic-scsynth dependency"

sedi "s/\"supersonic-scsynth-synthdefs\": \"\\^$CURRENT_VERSION\"/\"supersonic-scsynth-synthdefs\": \"^$NEW_VERSION\"/" "$PROJECT_ROOT/packages/supersonic-scsynth-bundle/package.json"
echo "✓ Updated supersonic-scsynth-synthdefs dependency"

sedi "s/\"supersonic-scsynth-samples\": \"\\^$CURRENT_VERSION\"/\"supersonic-scsynth-samples\": \"^$NEW_VERSION\"/" "$PROJECT_ROOT/packages/supersonic-scsynth-bundle/package.json"
echo "✓ Updated supersonic-scsynth-samples dependency"

sedi "s/\"supersonic-scsynth-core\": \"\\^$CURRENT_VERSION\"/\"supersonic-scsynth-core\": \"^$NEW_VERSION\"/" "$PROJECT_ROOT/packages/supersonic-scsynth-bundle/package.json"
echo "✓ Updated supersonic-scsynth-core dependency"

echo ""
echo "Step 3: Updating CDN constants and docs..."
echo "-------------------------------------------"

# Update CDN constants and docs
sedi "s|supersonic-scsynth-synthdefs@$CURRENT_VERSION|supersonic-scsynth-synthdefs@$NEW_VERSION|g" "$PROJECT_ROOT/packages/supersonic-scsynth-synthdefs/index.js"
echo "✓ Updated packages/supersonic-scsynth-synthdefs/index.js"

sedi "s|supersonic-scsynth-samples@$CURRENT_VERSION|supersonic-scsynth-samples@$NEW_VERSION|g" "$PROJECT_ROOT/packages/supersonic-scsynth-samples/index.js"
echo "✓ Updated packages/supersonic-scsynth-samples/index.js"

# every package the docs pin, not only the client: -core, -synthdefs and -samples too. A version in prose
# ("up to 0.81.0") has no @ before it and stays as the history it is.
sedi "s|@$CURRENT_VERSION|@$NEW_VERSION|g" "$PROJECT_ROOT/docs/INSTALLATION_WEB.md"
echo "✓ Updated docs/INSTALLATION_WEB.md"

echo ""
echo "Step 4: Updating C++ version constants..."
echo "------------------------------------------"

# Parse new version into components
IFS='.' read -r NEW_MAJOR NEW_MINOR NEW_PATCH <<< "$NEW_VERSION"

# The C++ side takes its version from CMake: project(SuperSonic VERSION x.y.z)
# in CMakeLists.txt, which clockwork turns into the boot banner, `-v`, and the
# Windows version resource.
sedi "s/^\(    VERSION \)$CURRENT_VERSION\$/\1$NEW_VERSION/" "$PROJECT_ROOT/CMakeLists.txt"
grep -q "VERSION $NEW_VERSION" "$PROJECT_ROOT/CMakeLists.txt" || { echo "ERROR: CMakeLists.txt VERSION was not $CURRENT_VERSION" >&2; exit 1; }
echo "✓ Updated CMakeLists.txt ($CURRENT_VERSION → $NEW_VERSION)"

echo ""
echo "Step 4b: Updating what else names the version..."
echo "-------------------------------------------------"

# The lock file's own version fields (npm keeps them in step with package.json)
npm install --package-lock-only --ignore-scripts --silent --prefix "$PROJECT_ROOT" >/dev/null
echo "✓ Updated package-lock.json"

# The examples pin a version in their CDN URLs, so they keep working against the release they were written for
for html in "$PROJECT_ROOT"/example/*.html; do
    [ -e "$html" ] || continue
    if grep -q "@$CURRENT_VERSION" "$html"; then
        sedi "s|@$CURRENT_VERSION|@$NEW_VERSION|g" "$html"
        echo "✓ Updated $(basename "$html")"
    fi
done

# Debian's changelog takes a new entry, not a rewritten one
CHANGELOG="$PROJECT_ROOT/packaging/debian/changelog"
if [ -f "$CHANGELOG" ] && ! head -1 "$CHANGELOG" | grep -q "($NEW_VERSION-1)"; then
    { printf 'supersonic (%s-1) unstable; urgency=medium\n\n  * Release %s\n\n -- %s <%s>  %s\n\n' \
        "$NEW_VERSION" "$NEW_VERSION" "$(git config user.name)" "$(git config user.email)" "$(date -R)"; cat "$CHANGELOG"; } > "$CHANGELOG.new"
    mv "$CHANGELOG.new" "$CHANGELOG"
    echo "✓ Added packaging/debian/changelog entry"
fi

# A version named in prose about what an older release did is history: left alone (js/supersonic.js, the specs).

echo ""
echo "Step 5: Regenerating manifests..."
echo "----------------------------------"

node "$PROJECT_ROOT/scripts/generate-manifests.mjs"
echo "✓ Manifests regenerated"

echo ""
echo "Step 6: Rebuilding distribution..."
echo "-----------------------------------"

if [ "$DO_BUILD" = true ]; then
    "$PROJECT_ROOT/scripts/build-web.sh" --release
    echo "✓ Build complete"
else
    echo "(skipped: npm publish builds --release itself)"
fi

echo ""
echo "Step 7: Committing changes..."
echo "------------------------------"

git add -u   # what the bump changed; untracked work in the tree is its owner's
if [ "$DO_COMMIT" = true ]; then
    git commit -m "Version - $NEW_VERSION"
    echo "✓ Changes committed"
    git tag "v$NEW_VERSION"
    echo "✓ Tagged v$NEW_VERSION"
else
    echo "✓ Changes staged (commit and tag v$NEW_VERSION by hand)"
fi

echo ""
echo -e "${GREEN}========================================"
echo "Version bump complete! 🎉"
echo "========================================${NC}"
echo ""
echo "New version: $NEW_VERSION"
echo ""
echo "Next steps:"
echo "  1. Review changes: git show"
echo "  2. Push to remote: git push --tags"
echo "  3. Publish to npm: ./publish.sh"
echo ""
