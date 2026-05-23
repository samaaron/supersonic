#!/bin/sh
# Apply Link patches idempotently inside the abletonlink-src tree.
# Invoked by CMake's PATCH_COMMAND every time the patch step runs.
#
# Behaviour:
#   - Patches already applied (reversible)      → skip silently
#   - Patches don't apply yet (fresh extract)   → apply them
#   - Mixed state, e.g. a patch file was edited → fail with a clear
#     message that points at the cache directory to clear
#
# Without this, CMake re-runs `git apply` every time the patch step
# is invalidated (e.g. when CMakeLists.txt changes how it calls
# PATCH_COMMAND, or when the patch files themselves change). On the
# second run `git apply` rejects with "patch does not apply" because
# the source has already been modified. That's the cache-clear gotcha
# this script avoids.
set -eu

# --ignore-whitespace: on Windows runners, git-for-Windows often
# checks Link's source out with CRLF (core.autocrlf=true) while the
# patches are LF. Context lines won't match without this flag.
# No-op on macOS / Linux where line endings already agree.

# Quick check: are all patches already applied? (reversible means applied.)
if git apply --ignore-whitespace --reverse --check "$@" 2>/dev/null; then
    echo "[supersonic-link-patches] already applied — skipping"
    exit 0
fi

# Not applied yet: try the forward direction.
if git apply --ignore-whitespace --check "$@" 2>/dev/null; then
    echo "[supersonic-link-patches] applying $#"
    git apply --ignore-whitespace --whitespace=nowarn "$@"
    exit 0
fi

# Mixed or partial state — the only safe fix is a fresh extract.
echo "[supersonic-link-patches] ERROR: patches don't apply cleanly to this Link source." >&2
echo "  This usually happens when a patch file was edited but the cached" >&2
echo "  abletonlink source still has the old version applied. Clear the cache:" >&2
echo "    rm -rf <build>/_deps/abletonlink-{src,build,subbuild}" >&2
echo "  Or for sonic-pi's nested build:" >&2
echo "    rm -rf <sonic-pi-build>/external/supersonic-prefix/src/supersonic-build/_deps/abletonlink-{src,build,subbuild}" >&2
exit 1
