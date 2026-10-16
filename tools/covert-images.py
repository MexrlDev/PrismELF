#!/usr/bin/env python3
"""covert-images.py - make sure assets/{logo,bg,splashbg,file,folder,elf,audio,image,video,text,archive}.rgba always exist.

For each required image:
  1. If images/<name>.(png|jpg|jpeg|bmp|gif|webp|tga) exists -> converted to the exact size the
     app needs and written to assets/<name>.rgba   (any size/format is fine)
  2. else, if assets/<name>.rgba already exists -> kept as is
  3. else -> a default placeholder is generated into assets/
Needs Pillow.   Usage: python3 tools/covert-images.py [--images images] [--assets assets] [--keep-going]

Sizes / fit:  splashbg 256x224 (fills, centre-cropped) | logo 256x128 (keeps shape, transparent padding) | bg 128x112 (fills, centre-cropped)
              file, folder and elf 16x16 (keep shape, transparent padding)
"""
import argparse, math, os, random, sys

try:
    from PIL import Image, ImageDraw
except ImportError:
    print("[covert-images] ERROR: Pillow is required (apk add py3-pillow / pip install pillow)")
    sys.exit(1)

EXTS = (".png", ".jpg", ".jpeg", ".bmp", ".gif", ".webp", ".tga")
SPEC = {  # name: (width, height, mode, aliases)
    "logo":   (256, 128, "contain", ("logo",)),
    "bg":     (128, 112, "cover",   ("bg", "background")),
    "file":   (16, 16,   "contain", ("file",)),
    "folder": (16, 16,   "contain", ("folder",)),
    "splashbg": (256, 224, "cover", ("splashbg", "splash_bg", "splashbackground")),   # dark backdrop behind the splash logo
    "elf":    (16, 16,   "contain", ("elf",)),
    "audio":  (16, 16,   "contain", ("audio",)),
    "image":  (16, 16,   "contain", ("image",)),
    "video":  (16, 16,   "contain", ("video",)),
    "text":   (16, 16,   "contain", ("text",)),
    "archive": (16, 16,  "contain", ("archive",)),     # blue icon shown for .elf / .ELF files
}


def log(m):
    print(f"[covert-images] {m}", flush=True)


def find_user_image(images_dir, aliases):
    if not os.path.isdir(images_dir):
        return None
    files = {f.lower(): f for f in os.listdir(images_dir)}
    for a in aliases:
        for e in EXTS:
            if a + e in files:
                return os.path.join(images_dir, files[a + e])
    return None


def fit(im, w, h, mode):
    im = im.convert("RGBA")
    sw, sh = im.size
    if mode == "cover":                       # fill the box, crop the overflow
        s = max(w / sw, h / sh)
        nw, nh = max(1, round(sw * s)), max(1, round(sh * s))
        im = im.resize((nw, nh), Image.LANCZOS)
        l, t = (nw - w) // 2, (nh - h) // 2
        return im.crop((l, t, l + w, t + h))
    s = min(w / sw, h / sh)                   # contain: keep shape, pad with transparency
    nw, nh = max(1, round(sw * s)), max(1, round(sh * s))
    im = im.resize((nw, nh), Image.LANCZOS)
    out = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    out.paste(im, ((w - nw) // 2, (h - nh) // 2), im)
    return out


def default_image(name):
    w, h, _, _ = SPEC[name]
    if name == "splashbg":
        rnd = random.Random(7)
        im = Image.new("RGBA", (w, h)); px = im.load()
        cx, cy, rad = w / 2, h * 0.46, w * 0.62
        for y in range(h):
            for x in range(w):
                t = y / (h - 1)
                r, g, b = 5 + 7 * t, 9 + 15 * t, 26 + 34 * t                    # deep navy gradient
                d = math.hypot((x - cx) / rad, (y - cy) / (rad * 0.8))
                glow = max(0.0, 1.0 - d) ** 2                                   # soft blue glow behind the logo
                r += 26 * glow; g += 70 * glow; b += 125 * glow
                vx, vy = (x - cx) / (w / 2), (y - h / 2) / (h / 2)
                v = 1.0 - 0.38 * min(1.0, (vx * vx + vy * vy) / 2)              # vignette
                if y % 2 == 0:
                    v *= 0.96                                                   # faint scanlines
                px[x, y] = (int(min(255, r * v)), int(min(255, g * v)), int(min(255, b * v)), 255)
        for _ in range(28):                                                      # a few dim stars
            sx, sy = rnd.randrange(w), rnd.randrange(h)
            br = rnd.randrange(60, 140)
            px[sx, sy] = (br, br + 20, min(255, br + 70), 255)
        return im
    if name == "bg":
        im = Image.new("RGBA", (w, h)); px = im.load()
        for y in range(h):
            for x in range(w):
                t = x / w * 0.5 + y / h * 0.5
                px[x, y] = (int(14 + 30 * t), int(10 + 22 * (1 - t)), int(40 + 70 * t), 255)
        d = ImageDraw.Draw(im)
        for i in range(0, w, 16):
            d.line([(i, 0), (i + 40, h)], fill=(255, 255, 255, 14))
        return im
    im = Image.new("RGBA", (w, h), (0, 0, 0, 0)); d = ImageDraw.Draw(im)
    if name == "logo":
        cx, cy = 128, 52
        d.polygon([(cx, cy - 40), (cx - 44, cy + 30), (cx + 44, cy + 30)], outline=(255, 255, 255, 255), fill=(120, 90, 255, 140))
        d.line([(cx - 60, cy + 4), (cx - 14, cy - 6)], fill=(255, 255, 255, 255), width=2)
        for k, c in enumerate([(255, 70, 70), (255, 200, 60), (90, 230, 110), (70, 170, 255), (170, 100, 255)]):
            d.line([(cx + 10, cy - 6), (cx + 95, cy - 24 + k * 12)], fill=c + (255,), width=3)
        d.text((82, 100), "PrismELF  v1.0", fill=(255, 255, 255, 255))
    elif name == "elf":                                  # processor-chip look with a lightning bolt: unmistakable
        d.rectangle([3, 3, 12, 12], fill=(16, 44, 110, 255), outline=(90, 220, 255, 255))
        for i in (4, 7, 10):
            d.point([(1, i), (2, i), (13, i), (14, i), (i, 1), (i, 2), (i, 13), (i, 14)], fill=(90, 220, 255, 255))
        d.polygon([(9, 4), (5, 8), (7, 8), (6, 11), (11, 7), (8, 7)], fill=(255, 220, 60, 255))
    elif name == "audio":
        d.rectangle([1, 1, 14, 14], fill=(40, 170, 90, 255))
        d.ellipse([3, 9, 7, 13], fill=(255, 255, 255, 255)); d.line([(7, 11), (7, 3)], fill=(255, 255, 255, 255), width=1)
        d.line([(7, 3), (12, 4)], fill=(255, 255, 255, 255), width=2)
    elif name == "image":
        d.rectangle([1, 2, 14, 13], fill=(30, 150, 170, 255)); d.rectangle([2, 3, 13, 12], fill=(120, 200, 240, 255))
        d.polygon([(2, 12), (6, 6), (9, 10), (11, 8), (13, 12)], fill=(40, 120, 70, 255)); d.ellipse([9, 4, 11, 6], fill=(255, 230, 90, 255))
    elif name == "video":
        d.rectangle([1, 2, 14, 13], fill=(230, 120, 30, 255))
        for y in (3, 6, 9, 12): d.point([(2, y), (13, y)], fill=(60, 30, 5, 255))
        d.polygon([(6, 5), (6, 10), (11, 7)], fill=(255, 255, 255, 255))
    elif name == "text":
        d.rectangle([3, 1, 12, 14], fill=(255, 244, 190, 255)); d.polygon([(9, 1), (12, 4), (9, 4)], fill=(200, 180, 110, 255))
        for y in (6, 8, 10, 12): d.line([(5, y), (10, y)], fill=(90, 70, 30, 255))
    elif name == "archive":
        d.rectangle([2, 2, 13, 14], fill=(170, 120, 70, 255)); d.rectangle([7, 2, 8, 14], fill=(235, 215, 150, 255))
        for y in (4, 7, 10): d.rectangle([6, y, 9, y + 1], fill=(60, 40, 20, 255))
    elif name == "folder":
        d.rectangle([1, 4, 14, 13], fill=(255, 200, 60, 255)); d.rectangle([1, 2, 6, 5], fill=(255, 200, 60, 255))
    else:
        d.rectangle([3, 1, 12, 14], fill=(230, 230, 240, 255)); d.polygon([(9, 1), (12, 4), (9, 4)], fill=(150, 150, 170, 255))
        for y in (7, 9, 11):
            d.line([(5, y), (10, y)], fill=(120, 120, 140, 255))
    return im


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--images", default="images")
    ap.add_argument("--assets", default="assets")
    ap.add_argument("--keep-going", action="store_true", help="on a bad image, warn and use the old/default asset")
    a = ap.parse_args()
    os.makedirs(a.assets, exist_ok=True)
    os.makedirs(a.images, exist_ok=True)
    failed = 0
    for name, (w, h, mode, aliases) in SPEC.items():
        out = os.path.join(a.assets, name + ".rgba")
        src = find_user_image(a.images, aliases)
        try:
            if src:
                im = fit(Image.open(src), w, h, mode)
                with open(out, "wb") as f:
                    f.write(im.tobytes())
                log(f"{os.path.basename(src)} -> {out} ({w}x{h}, {mode})")
            elif os.path.isfile(out) and os.path.getsize(out) == w * h * 4:
                log(f"no image for '{name}' in {a.images}/ -> keeping existing {out}")
            else:
                im = default_image(name)
                with open(out, "wb") as f:
                    f.write(im.tobytes())
                log(f"'{name}' missing everywhere -> generated default {out}")
        except Exception as e:                      # noqa: BLE001
            log(f"ERROR on '{name}': {e}")
            failed += 1
            if args_keep(a) and not (os.path.isfile(out) and os.path.getsize(out) == w * h * 4):
                with open(out, "wb") as f:
                    f.write(default_image(name).tobytes())
    for name, (w, h, _, _) in SPEC.items():          # final guarantee: all four exist with the right size
        p = os.path.join(a.assets, name + ".rgba")
        if not (os.path.isfile(p) and os.path.getsize(p) == w * h * 4):
            log(f"ERROR: {p} is missing or has the wrong size")
            return 1
    return 0 if (not failed or a.keep_going) else 1


def args_keep(a):
    return a.keep_going


if __name__ == "__main__":
    sys.exit(main())
