#!/usr/bin/env bash
# Lift the game's code from YOUR decrypted executable into work/: the PPU
# into work/src/recomp, the SPU images into work/port/spu. Nothing produced
# here may be shared: it is the game's code, translated.
#
#   scripts/lift.sh [path/to/EBOOT.elf]   (default: disc/PS3_GAME/USRDIR/EBOOT.elf)
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DIR="${PS3RECOMP_DIR:-$ROOT/ps3recomp}"
PY="${PYTHON:-$ROOT/.venv/bin/python}"
ELF="${1:-$ROOT/disc/PS3_GAME/USRDIR/EBOOT.elf}"
# The addresses in this port are those of BCUS98125 version 01.00.
EXPECT=bd4569d249e901449f2e0f6e3a289387af48cb02f637ba8ef950c13c5eb8c2a0

[ -f "$ELF" ] || { echo "no decrypted executable at $ELF (see README: Dump and decrypt)" >&2; exit 1; }
[ -d "$DIR/tools" ] || { echo "no toolkit at $DIR -- run scripts/setup.sh" >&2; exit 1; }
got=$(sha256sum "$ELF" | cut -c1-64)
if [ "$got" != "$EXPECT" ]; then
    echo "warning: $ELF is not the BCUS98125 01.00 executable this port was made for" >&2
    echo "         (sha256 $got); the lift may build but the port will not run it" >&2
fi

name=$(basename "$ELF"); name="${name%.*}"
mkdir -p "$ROOT/work/out" "$ROOT/work/src"
"$PY" "$DIR/tools/ppu_loader.py" "$ELF" -o "$ROOT/work/out"
"$PY" "$DIR/tools/ppu_lifter.py" "$ELF" \
    --functions "$ROOT/work/out/$name.functions.json" \
    --hle-stubs "$ROOT/work/out/$name.imports.json" \
    --code-end 0x8c96e8 --output "$ROOT/work/src/recomp"
"$PY" "$ROOT/work/tools/lift_spu.py" --toolkit "$DIR" --eboot "$ELF" --spu-dir "$ROOT/work/port/spu"
echo "lifted: work/src/recomp (PPU), work/port/spu (SPU)"
