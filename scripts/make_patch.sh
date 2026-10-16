#!/usr/bin/env bash
# Regenerate work/patches/ps3recomp.patch from a working ps3recomp tree: every
# change against the upstream base commit, minus build trees, firmware the
# toolkit can lift locally (fw_spu/), save data, local tool configuration and
# the conformance driver tools/test_ppu_lift.py regenerates on every run.
#
#   scripts/make_patch.sh [toolkit dir]   (default: $PS3RECOMP_DIR, else ./ps3recomp)
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
UPSTREAM=https://github.com/sp00nznet/ps3recomp.git
BASE=42f5f8f9415117f324ca438c46573f2caca63468
SRC="${1:-${PS3RECOMP_DIR:-$ROOT/ps3recomp}}"
[ -d "$SRC/tools" ] || { echo "no toolkit at $SRC" >&2; exit 1; }
tmp=$(mktemp -d)
trap 'rm -rf "${tmp:?}"' EXIT
git clone -q --no-checkout "$UPSTREAM" "$tmp/up"
git -C "$tmp/up" checkout -q "$BASE"
rsync -a --delete --exclude .git --exclude fw_spu --exclude gamedata --exclude build \
    --exclude 'build-*' --exclude __pycache__ --exclude .ruff_cache --exclude '.mcp.json' \
    --exclude 'mcp.json' --exclude '.env' --exclude '.env.*' \
    --exclude '/scratch/ppu_conformance*' "$SRC/" "$tmp/up/"
git -C "$tmp/up" add -A
git -C "$tmp/up" diff --cached > "$ROOT/work/patches/ps3recomp.patch"
git -C "$tmp/up" diff --cached --stat | tail -1
