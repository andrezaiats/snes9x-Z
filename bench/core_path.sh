#!/usr/bin/env bash
#
# core_path.sh - print the path of the libretro core for this platform, so
# a one-off command doesn't have to hardcode .so/.dylib/.dll.
#
#   ./bench/core_path.sh          # resolved core path (fails if not built)
#   ./bench/core_path.sh --jobs   # usable CPU count, for `make -j`
#
# --jobs deliberately does not require the core to exist, because it's also
# used to build the core in the first place.
#
# Usage:
#   cp "$(./bench/core_path.sh)" /tmp/patched.core
#   make -C libretro -j"$(./bench/core_path.sh --jobs)"

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

# lib.sh is linted on its own; shellcheck only follows sources under -x.
# shellcheck source=bench/lib.sh disable=SC1091
. "$SCRIPT_DIR/lib.sh"

case "${1:-}" in
    "")
        s9x_resolve_core "$REPO_DIR" "$(basename "$0")" || exit 1
        ;;
    --jobs)
        s9x_ncpu
        ;;
    *)
        echo "usage: $0 [--jobs]" >&2
        exit 2
        ;;
esac
