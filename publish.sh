#!/bin/bash
set -e

# Color output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

DRY_RUN=false
if [ "$1" = "--dry-run" ]; then
    DRY_RUN=true
    echo -e "${YELLOW}DRY RUN MODE - No packages will be published${NC}"
    echo ""
fi

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PACKAGES_DIR="$PROJECT_ROOT/packages"

publish_package() {
    local dir=$1
    local name=$2

    echo -e "${YELLOW}Publishing $name...${NC}"
    cd "$dir"

    if [ "$DRY_RUN" = true ]; then
        npm pack --dry-run
        echo -e "${GREEN}✓ $name (dry-run)${NC}"
    else
        if npm publish --access public 2>&1 | tee /tmp/npm-publish.log; then
            echo -e "${GREEN}✓ $name published${NC}"
        else
            if grep -q "cannot publish over the previously published" /tmp/npm-publish.log || grep -q "You cannot publish over" /tmp/npm-publish.log; then
                echo -e "${YELLOW}⊙ $name already published, skipping${NC}"
            else
                echo -e "${RED}✗ $name failed${NC}"
                cat /tmp/npm-publish.log
                return 1
            fi
        fi
    fi
    echo ""
}

echo "========================================"
echo "  SuperSonic npm Publishing Script"
echo "========================================"
echo ""

# Check if logged in
if ! npm whoami &> /dev/null; then
    echo -e "${RED}Error: Not logged in to npm. Run 'npm login' first.${NC}"
    exit 1
fi

echo -e "${GREEN}Logged in as: $(npm whoami)${NC}"
echo ""

# Step 1: Publish core package
echo "Step 1: Publishing core package..."
echo "-----------------------------------"
publish_package "$PROJECT_ROOT" "supersonic-scsynth"

# Step 2: Publish synthdefs
echo "Step 2: Publishing synthdefs package..."
echo "----------------------------------------"
publish_package "$PACKAGES_DIR/supersonic-scsynth-synthdefs" "supersonic-scsynth-synthdefs"

# Step 3: Publish unified samples package
echo "Step 3: Publishing unified samples package..."
echo "----------------------------------------------"
publish_package "$PACKAGES_DIR/supersonic-scsynth-samples" "supersonic-scsynth-samples"

# Step 4: Publish bundle
echo "Step 4: Publishing bundle package..."
echo "-------------------------------------"
publish_package "$PACKAGES_DIR/supersonic-scsynth-bundle" "supersonic-scsynth-bundle"

echo "========================================"
if [ "$DRY_RUN" = true ]; then
    echo -e "${GREEN}Dry run complete! All 4 packages validated.${NC}"
    echo ""
    echo "To publish for real, run: ./publish.sh"
else
    # Read version from package.json
    VERSION=$(node -p "require('./package.json').version")

    echo -e "${GREEN}All 4 packages published successfully! 🎉${NC}"
    echo ""
    echo "Packages available on CDN:"
    echo "  - https://unpkg.com/supersonic-scsynth@$VERSION"
    echo "  - https://unpkg.com/supersonic-scsynth-synthdefs@$VERSION"
    echo "  - https://unpkg.com/supersonic-scsynth-samples@$VERSION"
    echo "  - https://unpkg.com/supersonic-scsynth-bundle@$VERSION"
fi
echo "========================================"
