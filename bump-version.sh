#!/bin/bash
set -e

# SuperSonic Version Bump Script
#
# This script updates version numbers in exactly 11 locations:
#
# Package.json version fields (4):
#   1. package.json
#   2. packages/supersonic-scsynth-synthdefs/package.json
#   3. packages/supersonic-scsynth-samples/package.json
#   4. packages/supersonic-scsynth-bundle/package.json
#
# Bundle dependencies (3):
#   5. packages/supersonic-scsynth-bundle/package.json - supersonic-scsynth dependency
#   6. packages/supersonic-scsynth-bundle/package.json - supersonic-scsynth-synthdefs dependency
#   7. packages/supersonic-scsynth-bundle/package.json - supersonic-scsynth-samples dependency
#
# CDN constants in index.js (3):
#   8. packages/supersonic-scsynth-synthdefs/index.js - CDN_BASE constant
#   9. packages/supersonic-scsynth-samples/index.js - UNPKG_BASE constant
#  10. packages/supersonic-scsynth-samples/index.js - JSDELIVR_BASE constant
#
# C++ version constants (1):
#  11. src/audio_processor.cpp - SUPERSONIC_VERSION_MINOR constant
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
    echo "Usage: ./bump-version.sh <version>"
    echo "Example: ./bump-version.sh 0.1.2"
    exit 1
fi

NEW_VERSION=$1
PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

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
read -p "Continue with version bump? (y/n) " -n 1 -r
echo
if [[ ! $REPLY =~ ^[Yy]$ ]]; then
    echo "Aborted."
    exit 0
fi

echo ""
echo "Step 1: Updating package.json files..."
echo "---------------------------------------"

# Update 4 package.json version fields
sed -i "s/\"version\": \"$CURRENT_VERSION\"/\"version\": \"$NEW_VERSION\"/" "$PROJECT_ROOT/package.json"
echo "✓ Updated package.json"

sed -i "s/\"version\": \"$CURRENT_VERSION\"/\"version\": \"$NEW_VERSION\"/" "$PROJECT_ROOT/packages/supersonic-scsynth-synthdefs/package.json"
echo "✓ Updated packages/supersonic-scsynth-synthdefs/package.json"

sed -i "s/\"version\": \"$CURRENT_VERSION\"/\"version\": \"$NEW_VERSION\"/" "$PROJECT_ROOT/packages/supersonic-scsynth-samples/package.json"
echo "✓ Updated packages/supersonic-scsynth-samples/package.json"

sed -i "s/\"version\": \"$CURRENT_VERSION\"/\"version\": \"$NEW_VERSION\"/" "$PROJECT_ROOT/packages/supersonic-scsynth-bundle/package.json"
echo "✓ Updated packages/supersonic-scsynth-bundle/package.json"

echo ""
echo "Step 2: Updating bundle dependencies..."
echo "----------------------------------------"

# Update 3 bundle dependencies (use caret dependencies)
sed -i "s/\"supersonic-scsynth\": \"\\^$CURRENT_VERSION\"/\"supersonic-scsynth\": \"^$NEW_VERSION\"/" "$PROJECT_ROOT/packages/supersonic-scsynth-bundle/package.json"
echo "✓ Updated supersonic-scsynth dependency"

sed -i "s/\"supersonic-scsynth-synthdefs\": \"\\^$CURRENT_VERSION\"/\"supersonic-scsynth-synthdefs\": \"^$NEW_VERSION\"/" "$PROJECT_ROOT/packages/supersonic-scsynth-bundle/package.json"
echo "✓ Updated supersonic-scsynth-synthdefs dependency"

sed -i "s/\"supersonic-scsynth-samples\": \"\\^$CURRENT_VERSION\"/\"supersonic-scsynth-samples\": \"^$NEW_VERSION\"/" "$PROJECT_ROOT/packages/supersonic-scsynth-bundle/package.json"
echo "✓ Updated supersonic-scsynth-samples dependency"

echo ""
echo "Step 3: Updating CDN constants in index.js files..."
echo "----------------------------------------------------"

# Update 3 CDN constants
sed -i "s|supersonic-scsynth-synthdefs@$CURRENT_VERSION|supersonic-scsynth-synthdefs@$NEW_VERSION|g" "$PROJECT_ROOT/packages/supersonic-scsynth-synthdefs/index.js"
echo "✓ Updated packages/supersonic-scsynth-synthdefs/index.js"

sed -i "s|supersonic-scsynth-samples@$CURRENT_VERSION|supersonic-scsynth-samples@$NEW_VERSION|g" "$PROJECT_ROOT/packages/supersonic-scsynth-samples/index.js"
echo "✓ Updated packages/supersonic-scsynth-samples/index.js"

echo ""
echo "Step 4: Updating C++ version constants..."
echo "------------------------------------------"

# Parse new version into components
IFS='.' read -r NEW_MAJOR NEW_MINOR NEW_PATCH <<< "$NEW_VERSION"

# Get current C++ version components
CURRENT_CPP_MAJOR=$(grep "SUPERSONIC_VERSION_MAJOR" "$PROJECT_ROOT/src/audio_processor.cpp" | grep -o '[0-9]\+')
CURRENT_CPP_MINOR=$(grep "SUPERSONIC_VERSION_MINOR" "$PROJECT_ROOT/src/audio_processor.cpp" | grep -o '[0-9]\+')
CURRENT_CPP_PATCH=$(grep "SUPERSONIC_VERSION_PATCH" "$PROJECT_ROOT/src/audio_processor.cpp" | grep -o '[0-9]\+')

# Update C++ version (all three components)
sed -i "s/static const int SUPERSONIC_VERSION_MAJOR = $CURRENT_CPP_MAJOR;/static const int SUPERSONIC_VERSION_MAJOR = $NEW_MAJOR;/" "$PROJECT_ROOT/src/audio_processor.cpp"
sed -i "s/static const int SUPERSONIC_VERSION_MINOR = $CURRENT_CPP_MINOR;/static const int SUPERSONIC_VERSION_MINOR = $NEW_MINOR;/" "$PROJECT_ROOT/src/audio_processor.cpp"
sed -i "s/static const int SUPERSONIC_VERSION_PATCH = $CURRENT_CPP_PATCH;/static const int SUPERSONIC_VERSION_PATCH = $NEW_PATCH;/" "$PROJECT_ROOT/src/audio_processor.cpp"
echo "✓ Updated src/audio_processor.cpp ($CURRENT_CPP_MAJOR.$CURRENT_CPP_MINOR.$CURRENT_CPP_PATCH → $NEW_VERSION)"

echo ""
echo "Step 5: Rebuilding distribution..."
echo "-----------------------------------"

# Run build with --release flag
./build.sh --release
echo "✓ Build complete"

echo ""
echo "Step 6: Committing changes..."
echo "------------------------------"

# Stage all changes
git add -A

# Create commit
git commit -m "Version - $NEW_VERSION"
echo "✓ Changes committed"

# Create git tag
git tag "v$NEW_VERSION"
echo "✓ Tagged v$NEW_VERSION"

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
