#!/usr/bin/env bash
# ============================================================================
# build-all.sh — Build php-trace extension for all PHP versions
#
# Usage:
#   chmod +x docker/build-all.sh
#   ./docker/build-all.sh              # Build all versions
#   ./docker/build-all.sh 8.1 8.3      # Build specific versions
#   ./docker/build-all.sh --push       # Build and push to registry
# ============================================================================

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
REGISTRY="${REGISTRY:-}"
PUSH="${PUSH:-false}"

# Colors
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
RED='\033[0;31m'
CYAN='\033[0;36m'
NC='\033[0m'

# Parse args
VERSIONS=()
while [[ $# -gt 0 ]]; do
    case "$1" in
        --push) PUSH=true; shift ;;
        --registry) REGISTRY="$2"; shift 2 ;;
        *) VERSIONS+=("$1"); shift ;;
    esac
done

# Default: all supported versions
if [[ ${#VERSIONS[@]} -eq 0 ]]; then
    VERSIONS=("8.0" "8.1" "8.2" "8.3" "8.4")
fi

# Tag prefix
if [[ -n "$REGISTRY" ]]; then
    TAG_PREFIX="${REGISTRY}/"
else
    TAG_PREFIX=""
fi

# ============================================================================
# Build a single version
# ============================================================================
build_version() {
    local ver="$1"
    local tag="${TAG_PREFIX}php-trace:${ver}"
    
    echo -e "${CYAN}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"
    echo -e "${CYAN}  Building php-trace for PHP ${ver}${NC}"
    echo -e "${CYAN}  Tag: ${tag}${NC}"
    echo -e "${CYAN}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"
    
    if docker build \
        -f "${SCRIPT_DIR}/Dockerfile" \
        --build-arg "PHP_VERSION=${ver}" \
        -t "${tag}" \
        "${PROJECT_DIR}"; then
        echo -e "${GREEN}✓ PHP ${ver} build succeeded${NC}"
        
        # Verify by running php --ri
        echo ""
        echo -e "${YELLOW}── Verification: php --ri php_trace ──${NC}"
        docker run --rm "${tag}" php --ri php_trace 2>/dev/null || true
        echo ""
    else
        echo -e "${RED}✗ PHP ${ver} build FAILED${NC}"
        return 1
    fi
}

# ============================================================================
# Push a version to registry
# ============================================================================
push_version() {
    local ver="$1"
    local tag="${TAG_PREFIX}php-trace:${ver}"
    
    if [[ "$PUSH" == "true" ]]; then
        echo -e "${YELLOW}  Pushing ${tag}...${NC}"
        docker push "${tag}"
    fi
}

# ============================================================================
# Main
# ============================================================================
echo -e "${CYAN}"
echo "╔══════════════════════════════════════════════════════════╗"
echo "║     php-trace — Multi-Version Docker Build              ║"
echo "╠══════════════════════════════════════════════════════════╣"
printf "║  Versions: %-46s ║\n" "${VERSIONS[*]}"
echo "╚══════════════════════════════════════════════════════════╝"
echo -e "${NC}"

TOTAL=${#VERSIONS[@]}
SUCCESS=0
FAILED=0
START_TIME=$(date +%s)

for ver in "${VERSIONS[@]}"; do
    if build_version "$ver"; then
        push_version "$ver"
        ((SUCCESS++))
    else
        ((FAILED++))
    fi
done

END_TIME=$(date +%s)
ELAPSED=$((END_TIME - START_TIME))

echo ""
echo -e "${CYAN}╔══════════════════════════════════════════════════════════╗${NC}"
printf "${CYAN}║${NC}  ${GREEN}Success: %d${NC}  ${RED}Failed: %d${NC}  Time: %dm%ds ${CYAN}               ║${NC}\n" \
    "$SUCCESS" "$FAILED" $((ELAPSED/60)) $((ELAPSED%60))
echo -e "${CYAN}╚══════════════════════════════════════════════════════════╝${NC}"

exit $FAILED
