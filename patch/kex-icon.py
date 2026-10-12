#!/usr/bin/env python3
"""kex-icon.py: Notepad++'s own icon with a small "enhanced" badge, for Notepad++ KEX.

    python3 kex-icon.py SOURCE.ico OUT.ico [PREVIEW.png]

The badge (a white sparkle on a violet-to-blue disc with a white rim) sits in the bottom-right
corner, the empty space beside the pencil, and is drawn separately at every icon size so it stays
crisp at 16 px. Windows' shortcut arrow goes in the other corner. SOURCE is the icon of the
Notepad++ release being built (kex-patch.sh passes the unpatched icons/npp.ico), so a new upstream
icon gets the badge too. OUT has 32-bit images at 16-256 px: bitmaps up to 128 px, PNG at 256 px,
as in Notepad++'s own file. Needs Pillow.
"""
import io
import math
import struct
import sys

from PIL import Image, ImageDraw, ImageFilter

SIZES = [16, 20, 24, 32, 40, 48, 64, 128, 256]
SUPER = 8                                   # supersampling for the badge
TOP, BOTTOM = (168, 85, 247), (59, 130, 246)  # disc gradient: violet to blue


def base_image(ico, size):
    """The source icon's own image at this size, or the next larger one scaled down."""
    have = sorted(s[0] for s in ico.ico.sizes())
    src = next((s for s in have if s >= size), have[-1])
    im = ico.ico.getimage((src, src)).convert("RGBA")
    if src != size:
        im = im.convert("RGBa").resize((size, size), Image.LANCZOS).convert("RGBA")
    return im


def badge_geometry(size):
    """Disc radius and centre in pixels: relatively bigger on small icons so it stays visible. Up to
    24 px the disc has an odd diameter and touches the corner, so its edge and the sparkle's arms
    fall on whole pixels."""
    small = {16: 3.5, 20: 4.5, 24: 5.5}
    if size in small:
        r = small[size]
        return r, size - r
    frac = {32: 0.22, 40: 0.215, 48: 0.21, 64: 0.2, 128: 0.17}.get(size, 0.16)
    r = size * frac
    return r, size - r - size * 0.015


def sparkle(cx, cy, radius, steps=96, power=2.6):
    """A four-pointed star: a superellipse with an exponent above 1 has concave sides."""
    pts = []
    for i in range(steps):
        t = 2 * math.pi * i / steps
        c, s = math.cos(t), math.sin(t)
        x = math.copysign(abs(c) ** power, c)
        y = math.copysign(abs(s) ** power, s)
        pts.append((cx + radius * x, cy + radius * y))
    return pts


def badge(size):
    """The badge alone, as an RGBA layer of the icon's size."""
    big = size * SUPER
    r, centre = badge_geometry(size)
    R, C = r * SUPER, centre * SUPER
    layer = Image.new("RGBA", (big, big), (0, 0, 0, 0))
    if size >= 32:
        # a soft shadow under the disc separates it from the page and the pencil
        sh = Image.new("RGBA", (big, big), (0, 0, 0, 0))
        off = size * 0.012 * SUPER
        ImageDraw.Draw(sh).ellipse((C - R + off, C - R + off * 1.5, C + R + off, C + R + off * 1.5), fill=(0, 0, 0, 110))
        layer = Image.alpha_composite(layer, sh.filter(ImageFilter.GaussianBlur(size * 0.025 * SUPER)))
    d = ImageDraw.Draw(layer)
    # a white rim, except on the smallest sizes where it would read as a ring around a dot
    rim = 0.0 if size <= 24 else max(1.0, size * 0.03) * SUPER
    if rim:
        d.ellipse((C - R, C - R, C + R, C + R), fill=(255, 255, 255, 255))
    # the disc: a diagonal gradient, masked to a circle inside the rim
    inner = R - rim
    grad = Image.new("RGBA", (big, big))
    gp = grad.load()
    lo, hi = C - inner, C + inner
    for y in range(int(lo) - 1, int(hi) + 2):
        for x in range(int(lo) - 1, int(hi) + 2):
            if 0 <= x < big and 0 <= y < big:
                t = min(1.0, max(0.0, ((x - lo) + (y - lo)) / (2 * (hi - lo))))
                gp[x, y] = tuple(int(TOP[i] + (BOTTOM[i] - TOP[i]) * t) for i in range(3)) + (255,)
    mask = Image.new("L", (big, big), 0)
    ImageDraw.Draw(mask).ellipse((C - inner, C - inner, C + inner, C + inner), fill=255)
    layer.paste(grad, (0, 0), mask)
    # the sparkle, slightly above centre like a glint
    star = inner * (0.9 if size <= 24 else 0.72)
    d = ImageDraw.Draw(layer)
    d.polygon(sparkle(C, C - (0 if size <= 24 else inner * 0.02), star, power=2.9 if size <= 24 else 2.6),
              fill=(255, 255, 255, 255))
    if size >= 48:
        d.polygon(sparkle(C + inner * 0.5, C - inner * 0.48, star * 0.3), fill=(255, 255, 255, 235))
    return layer.resize((size, size), Image.LANCZOS)


def bmp_entry(im):
    """A 32-bit icon bitmap: BITMAPINFOHEADER, bottom-up BGRA rows, then the 1-bit AND mask."""
    w, h = im.size
    px = im.load()
    header = struct.pack("<IiiHHIIiiII", 40, w, h * 2, 1, 32, 0, 0, 0, 0, 0, 0)
    xor = bytearray()
    for y in range(h - 1, -1, -1):
        for x in range(w):
            r, g, b, a = px[x, y]
            xor += bytes((b, g, r, a))
    stride = ((w + 31) // 32) * 4
    mask = bytearray()
    for y in range(h - 1, -1, -1):
        row = bytearray(stride)
        for x in range(w):
            if px[x, y][3] < 128:
                row[x // 8] |= 0x80 >> (x % 8)
        mask += row
    return header + bytes(xor) + bytes(mask)


def png_entry(im):
    buf = io.BytesIO()
    im.save(buf, format="PNG", optimize=True)
    return buf.getvalue()


def write_ico(images, path):
    entries = [(im.size[0], png_entry(im) if im.size[0] >= 256 else bmp_entry(im)) for im in images]
    out = bytearray(struct.pack("<HHH", 0, 1, len(entries)))
    offset = 6 + 16 * len(entries)
    for size, data in entries:
        dim = 0 if size >= 256 else size
        out += struct.pack("<BBBBHHII", dim, dim, 0, 0, 1, 32, len(data), offset)
        offset += len(data)
    for _, data in entries:
        out += data
    with open(path, "wb") as f:
        f.write(out)


def preview(images, path):
    """Each size, actual size and enlarged 4x, on a light and a dark background."""
    pad = 12
    w = sum(s for s in SIZES) + pad * (len(SIZES) + 1)
    big = [im.resize((im.size[0] * 4, im.size[1] * 4), Image.NEAREST) for im in images if im.size[0] <= 32]
    w2 = sum(b.size[0] for b in big) + pad * (len(big) + 1)
    W = max(w, w2)
    H = (256 + pad * 2) + (128 + pad * 2)
    sheet = Image.new("RGBA", (W, H * 2), (0, 0, 0, 0))
    for row, bg in enumerate([(243, 243, 243, 255), (32, 32, 32, 255)]):
        y0 = row * H
        sheet.paste(Image.new("RGBA", (W, H), bg), (0, y0))
        x = pad
        for im in images:
            sheet.alpha_composite(im, (x, y0 + pad + 256 - im.size[1]))
            x += im.size[0] + pad
        x = pad
        for b in big:
            sheet.alpha_composite(b, (x, y0 + 256 + pad * 3))
            x += b.size[0] + pad
    sheet.save(path)


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    ico = Image.open(sys.argv[1])
    images = []
    for size in SIZES:
        im = base_image(ico, size)
        images.append(Image.alpha_composite(im, badge(size)))
    write_ico(images, sys.argv[2])
    if len(sys.argv) > 3:
        preview(images, sys.argv[3])


if __name__ == "__main__":
    main()
