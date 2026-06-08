#!/usr/bin/env bash
# sync_check.sh - Verify wire_format.h is identical between raft-l2-oracle and n3x-infrathon
#
# Usage: ./tools/sync_check.sh [--fix]
#   --fix: copy raft-l2-oracle's version to n3x-infrathon (source of truth)

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ORACLE_ROOT="$(dirname "$SCRIPT_DIR")"

SRC="$ORACLE_ROOT/firmware/protocol/wire_format.h"
DST="${N3X_INFRATHON_DIR:-$HOME/src/n3x-infrathon}/backends/debian/meta-n3x/recipes-oracle/oracle-agent/files/wire_format.h"

if [ ! -f "$SRC" ]; then
    echo "ERROR: source not found: $SRC" >&2
    exit 2
fi

if [ ! -f "$DST" ]; then
    echo "ERROR: destination not found: $DST" >&2
    echo "  Set N3X_INFRATHON_DIR if n3x-infrathon is not at ~/src/n3x-infrathon" >&2
    exit 2
fi

if diff -q "$SRC" "$DST" > /dev/null 2>&1; then
    echo "OK: wire_format.h is in sync"
    exit 0
else
    echo "DIVERGED: wire_format.h differs between projects"
    echo ""
    diff -u "$DST" "$SRC" || true
    echo ""
    if [ "${1:-}" = "--fix" ]; then
        command cp -f "$SRC" "$DST"
        echo "FIXED: copied raft-l2-oracle -> n3x-infrathon"
    else
        echo "Run with --fix to sync (raft-l2-oracle is source of truth)"
    fi
    exit 1
fi
