#!/usr/bin/env python3
"""Normalize per-system box-art thumbnails to one consistent footprint.

Usage: scripts/normalize-boxart.py <Roms-dir>

For each system folder's .res/ directory, every PNG is fitted onto a single
per-system transparent canvas so all thumbnails in a list occupy identical
space. The canvas leans small on purpose:

- height  = the smallest art height in that system (capped at 320, floored
  at 160) so nothing is ever upscaled -- upscaling is what makes art look
  stretched/blurry on the 640x480 screens
- width   = height * the system's median aspect ratio (each console keeps
  its native box shape: GB roughly square, PS jewel-case, etc.)
- images are only ever scaled DOWN to fit, then centered; the transparent
  padding keeps the footprint identical without distorting anything

Idempotent: images already at canvas size are skipped.
"""

import sys
from pathlib import Path
from statistics import median

from PIL import Image

CAP_H = 320
FLOOR_H = 160


def normalize_system(res_dir: Path) -> str:
    pngs = sorted(res_dir.glob("*.png"))
    if not pngs:
        return "no art"

    sizes = {}
    for p in pngs:
        try:
            with Image.open(p) as im:
                sizes[p] = im.size
        except Exception:
            print(f"  warn: unreadable, skipping {p.name}")

    if not sizes:
        return "no readable art"

    target_h = max(FLOOR_H, min(CAP_H, min(h for _, h in sizes.values())))
    target_w = round(target_h * median(w / h for w, h in sizes.values()))

    done = skipped = 0
    for p, (w, h) in sizes.items():
        if (w, h) == (target_w, target_h):
            skipped += 1
            continue
        with Image.open(p) as im:
            im = im.convert("RGBA")
            scale = min(target_w / w, target_h / h, 1.0)  # never upscale
            if scale < 1.0:
                im = im.resize((max(1, round(w * scale)), max(1, round(h * scale))), Image.LANCZOS)
            canvas = Image.new("RGBA", (target_w, target_h), (0, 0, 0, 0))
            canvas.paste(im, ((target_w - im.width) // 2, (target_h - im.height) // 2))
            canvas.save(p, optimize=True)
        done += 1
    return f"canvas {target_w}x{target_h}, normalized {done}, already-normal {skipped}"


def main() -> int:
    if len(sys.argv) != 2:
        print(__doc__)
        return 1
    roms = Path(sys.argv[1])
    if not roms.is_dir():
        print(f"error: {roms} is not a directory")
        return 1
    for system in sorted(d for d in roms.iterdir() if d.is_dir()):
        res = system / ".res"
        if res.is_dir():
            print(f"{system.name}: {normalize_system(res)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
