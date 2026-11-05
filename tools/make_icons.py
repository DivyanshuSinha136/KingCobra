#!/usr/bin/env python3
"""Regenerate every KingCobra icon from assets/icons/kingcobra-logo.jpg (needs Pillow).

The logo is a document-shaped icon, so it is used as-is as the icon for every .kc file:
  assets/icons/hicolor/<N>x<N>/mimetypes/text-x-kingcobra.png   (Linux file-type icon, freedesktop theme layout)
  assets/icons/hicolor/<N>x<N>/apps/kingcobra.png                (Linux application icon)
  assets/icons/kingcobra.png                                     (512 px master, transparent background)
  assets/icons/kingcobra.ico                                     (Windows: 16..256 px, embedded in kingcobra.exe too)
Run from the repository root:   python3 tools/make_icons.py      (or: make icons)
"""
import os, sys
from collections import deque
from PIL import Image

SRC = "assets/icons/kingcobra-logo.jpg"
OUT = "assets/icons"
SIZES = [16, 24, 32, 48, 64, 128, 256, 512]

def master():
    im = Image.open(SRC).convert("L")
    w, h = im.size
    px = im.load()
    # 1. everything light that touches the image border is background -> transparent
    outside = [[False] * w for _ in range(h)]
    q = deque()
    for x in range(w):
        for y in (0, h - 1):
            q.append((x, y))
    for y in range(h):
        for x in (0, w - 1):
            q.append((x, y))
    while q:
        x, y = q.popleft()
        if x < 0 or y < 0 or x >= w or y >= h or outside[y][x] or px[x, y] < 170:
            continue
        outside[y][x] = True
        q.extend(((x + 1, y), (x - 1, y), (x, y + 1), (x, y - 1)))
    rgba = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    out = rgba.load()
    for y in range(h):
        for x in range(w):
            if not outside[y][x]:
                v = px[x, y]
                out[x, y] = (v, v, v, 255)
    # 2. crop to the page and centre it on a square canvas
    box = rgba.getbbox()
    page = rgba.crop(box)
    side = max(page.size)
    pad = round(side * 0.03)
    canvas = Image.new("RGBA", (side + 2 * pad, side + 2 * pad), (0, 0, 0, 0))
    canvas.paste(page, ((canvas.width - page.width) // 2, (canvas.height - page.height) // 2))
    return canvas.resize((1024, 1024), Image.LANCZOS)

def main():
    if not os.path.exists(SRC):
        sys.exit("run from the repository root (missing %s)" % SRC)
    big = master()
    for n in SIZES:
        img = big.resize((n, n), Image.LANCZOS)
        for kind, name in (("mimetypes", "text-x-kingcobra.png"), ("apps", "kingcobra.png")):
            d = "%s/hicolor/%dx%d/%s" % (OUT, n, n, kind)
            os.makedirs(d, exist_ok=True)
            img.save("%s/%s" % (d, name), optimize=True)
    big.resize((512, 512), Image.LANCZOS).save(OUT + "/kingcobra.png", optimize=True)
    big.save(OUT + "/kingcobra.ico", sizes=[(s, s) for s in (16, 24, 32, 48, 64, 128, 256)])
    print("icons written to", OUT)

if __name__ == "__main__":
    main()
