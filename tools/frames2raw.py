#!/usr/bin/env python3
"""PNG 序列帧 -> RGB565 小端裸位图（固件直接贴屏）"""
import os, glob, sys
from PIL import Image

SRC = os.path.join(os.path.dirname(__file__), "..", "frames")
DST = os.path.join(os.path.dirname(__file__), "..", "build", "frames")
W, H = 1024, 576

def main():
    os.makedirs(DST, exist_ok=True)
    files = sorted(glob.glob(os.path.join(SRC, "*.png")))
    if not files:
        print("no frames", file=sys.stderr); return 1
    for f in files:
        idx = int(os.path.basename(f)[:3])
        im = Image.open(f).convert("RGB").resize((W, H), Image.LANCZOS)
        buf = bytearray()
        px = im.load()
        for y in range(H):
            for x in range(W):
                r, g, b = px[x, y]
                v = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)
                buf += v.to_bytes(2, "little")
        out = os.path.join(DST, f"{idx:03d}.bin")
        open(out, "wb").write(buf)
        print(f"{out} {len(buf)}")
    return 0

if __name__ == "__main__":
    raise SystemExit(main())
