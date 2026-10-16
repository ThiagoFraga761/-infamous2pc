#!/usr/bin/env python3
"""Binary PPM (P6) frame dumps -> PNG, half size, for looking at them.

Usage: work/tools/ppm2png.py [--gain G] [--scale S] frame.ppm [...]
Writes frame.png beside each input. --gain multiplies every channel (dark
HDR targets); --scale keeps one pixel in S per axis (default 2).
"""
import argparse
import struct
import zlib
from pathlib import Path


def read_ppm(path):
    d = Path(path).read_bytes()
    i = 0
    fields = []
    while len(fields) < 4:
        while d[i] in b" \t\r\n":
            i += 1
        j = i
        while d[j] not in b" \t\r\n":
            j += 1
        fields.append(d[i:j])
        i = j
    if fields[0] != b"P6":
        raise SystemExit(f"{path}: not a binary PPM")
    w, h = int(fields[1]), int(fields[2])
    return w, h, d[i + 1:i + 1 + w * h * 3]


def write_png(path, w, h, rows):
    def chunk(tag, body):
        return struct.pack(">I", len(body)) + tag + body + struct.pack(">I", zlib.crc32(tag + body))
    Path(path).write_bytes(b"\x89PNG\r\n\x1a\n"
                           + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
                           + chunk(b"IDAT", zlib.compress(b"".join(rows), 6))
                           + chunk(b"IEND", b""))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--gain", type=float, default=1.0)
    ap.add_argument("--scale", type=int, default=2)
    ap.add_argument("frames", nargs="+")
    args = ap.parse_args()
    lut = bytes(min(255, int(v * args.gain)) for v in range(256))
    for src in args.frames:
        w, h, px = read_ppm(src)
        s = max(1, args.scale)
        rows = []
        for y in range(0, h - h % s, s):
            line = px[y * w * 3:(y + 1) * w * 3]
            out = bytearray(b"\x00")
            for x in range(0, w - w % s, s):
                out += line[x * 3:x * 3 + 3]
            rows.append(bytes(out).translate(lut) if args.gain != 1.0 else bytes(out))
        dst = str(Path(src).with_suffix(".png"))
        write_png(dst, w // s, h // s, rows)
        print(dst)


if __name__ == "__main__":
    main()
