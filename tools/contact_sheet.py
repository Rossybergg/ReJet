"""Make a labelled contact sheet from the loader's screenshots (shot_NNNNN.bmp), for checking a
scripted run at a glance. Uses only the standard library (writes a BMP).

    python tools/contact_sheet.py build/loader [first_frame] [last_frame] [columns] [out.bmp]
"""
import glob
import os
import struct
import sys


def read_bmp(path):
    d = open(path, "rb").read()
    off, = struct.unpack_from("<I", d, 10)
    w, h, _, bpp = struct.unpack_from("<iiHH", d, 18)
    top_down = h < 0
    h = abs(h)
    stride = (w * bpp // 8 + 3) & ~3
    px = []
    for y in range(h):
        row = y if top_down else h - 1 - y
        base = off + row * stride
        px.append(d[base:base + w * bpp // 8])
    return w, h, bpp // 8, px


def thumb(path, tw, th):
    w, h, bpp, rows = read_bmp(path)
    out = bytearray(tw * th * 3)
    for y in range(th):
        src = rows[y * h // th]
        for x in range(tw):
            sx = (x * w // tw) * bpp
            out[(y * tw + x) * 3:(y * tw + x) * 3 + 3] = src[sx:sx + 3]
    return out


# 3x5 digit glyphs for the frame labels
DIGITS = ["111101101101111", "010110010010111", "111001111100111", "111001111001111", "101101111001001",
          "111100111001111", "111100111101111", "111001001001001", "111101111101111", "111101111001111"]


def label(img, iw, x0, y0, text, scale=3):
    for k, ch in enumerate(text):
        g = DIGITS[int(ch)]
        for gy in range(5):
            for gx in range(3):
                on = g[gy * 3 + gx] == "1"
                for sy in range(scale):
                    for sx in range(scale):
                        x = x0 + k * 4 * scale + gx * scale + sx
                        y = y0 + gy * scale + sy
                        i = (y * iw + x) * 3
                        img[i:i + 3] = b"\x00\xff\xff" if on else b"\x00\x00\x00"


def main():
    d = sys.argv[1]
    lo = int(sys.argv[2]) if len(sys.argv) > 2 else 0
    hi = int(sys.argv[3]) if len(sys.argv) > 3 else 10 ** 9
    cols = int(sys.argv[4]) if len(sys.argv) > 4 else 5
    out = sys.argv[5] if len(sys.argv) > 5 else os.path.join(d, "sheet.bmp")
    shots = sorted(p for p in glob.glob(os.path.join(d, "shot_*.bmp"))
                   if lo <= int(os.path.basename(p)[5:10]) <= hi)
    if not shots:
        sys.exit("no screenshots in range")
    tw, th = 256, 192
    rows = (len(shots) + cols - 1) // cols
    iw, ih = cols * tw, rows * th
    img = bytearray(iw * ih * 3)
    for n, p in enumerate(shots):
        t = thumb(p, tw, th)
        ox, oy = (n % cols) * tw, (n // cols) * th
        for y in range(th):
            i = ((oy + y) * iw + ox) * 3
            img[i:i + tw * 3] = t[y * tw * 3:(y + 1) * tw * 3]
        label(img, iw, ox + 4, oy + 4, os.path.basename(p)[5:10].lstrip("0") or "0")
    stride = (iw * 3 + 3) & ~3
    with open(out, "wb") as f:
        f.write(b"BM" + struct.pack("<IHHI", 54 + stride * ih, 0, 0, 54))
        f.write(struct.pack("<IiiHHIIiiII", 40, iw, -ih, 1, 24, 0, stride * ih, 2835, 2835, 0, 0))
        pad = b"\0" * (stride - iw * 3)
        for y in range(ih):
            f.write(img[y * iw * 3:(y + 1) * iw * 3] + pad)
    print(f"{len(shots)} shots -> {out}")


if __name__ == "__main__":
    main()
