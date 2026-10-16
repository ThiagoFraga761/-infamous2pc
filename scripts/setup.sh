#!/usr/bin/env bash
# Fetch ps3recomp at the commit the toolkit patch is made against, apply the
# patch, and install its Python tools into .venv.
#
#   scripts/setup.sh            toolkit in ./ps3recomp
#   PS3RECOMP_DIR=... scripts/setup.sh
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
UPSTREAM=https://github.com/sp00nznet/ps3recomp.git
BASE=42f5f8f9415117f324ca438c46573f2caca63468   # v0.13.0 docs, 2026-10-07
PATCH="$ROOT/work/patches/ps3recomp.patch"
DIR="${PS3RECOMP_DIR:-$ROOT/ps3recomp}"

if [ ! -d "$DIR/.git" ]; then
    git clone "$UPSTREAM" "$DIR"
fi
git -C "$DIR" checkout --quiet "$BASE"
if git -C "$DIR" apply --check "$PATCH" 2>/dev/null; then
    git -C "$DIR" apply "$PATCH"
    echo "patch applied to $DIR"
elif git -C "$DIR" apply --reverse --check "$PATCH" 2>/dev/null; then
    echo "patch already applied in $DIR"
else
    echo "the patch does not apply to $DIR at $BASE -- start from a clean clone" >&2
    exit 1
fi

python3 -m venv "$ROOT/.venv"
"$ROOT/.venv/bin/pip" install --quiet -r "$DIR/requirements.txt"
echo "toolkit ready: $DIR (python: $ROOT/.venv)"
