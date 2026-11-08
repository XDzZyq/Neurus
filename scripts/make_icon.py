"""Generate the multi-size Windows application icon from the source SVG.

The icon lives in the repository as both an SVG (embedded in the Qt resources
and used for the window/taskbar icon) and an ICO (compiled into Neurus.exe by
res/ui/logo/neurus.rc). The ICO cannot be derived at build time without adding
an SVG rasteriser to the toolchain, so it is committed and regenerated here
whenever the SVG changes:

    python scripts/make_icon.py

The renderer understands the subset of SVG the logo uses: a viewBox, a <style>
block mapping class selectors to solid fills, and <rect> elements. Each output
pixel is an exact area-weighted average of the rectangles covering it, which
gives clean antialiasing without an external library.
"""

import argparse
import struct
import xml.etree.ElementTree as ET
import zlib
from pathlib import Path

# Sizes Windows asks for: taskbar, title bar, Alt-Tab, Explorer, large icons.
SIZES = [16, 24, 32, 48, 64, 128, 256]

SVG_NS = "{http://www.w3.org/2000/svg}"
REPO_ROOT = Path(__file__).resolve().parent.parent
DEFAULT_SVG = REPO_ROOT / "res" / "ui" / "logo" / "neurus.svg"
DEFAULT_ICO = REPO_ROOT / "res" / "ui" / "logo" / "neurus.ico"


def parse_rects(svg_path):
    """Return (viewBox width, height, [(x, y, w, h, (r, g, b)), ...])."""
    root = ET.parse(svg_path).getroot()

    view = [float(v) for v in root.get("viewBox").split()]
    if len(view) != 4:
        raise ValueError(f"{svg_path}: expected a 4-value viewBox")

    fills = {}
    for style in root.iter(SVG_NS + "style"):
        for rule in (style.text or "").split("}"):
            if "{" not in rule:
                continue
            selector, body = rule.split("{", 1)
            selector = selector.strip()
            if not selector.startswith("."):
                continue
            for prop in body.strip().rstrip(";").split(";"):
                if prop.strip().startswith("fill:"):
                    fills[selector[1:]] = prop.split(":", 1)[1].strip()

    rects = []
    for rect in root.iter(SVG_NS + "rect"):
        color = fills[rect.get("class", "")].lstrip("#")
        if len(color) == 3:
            color = "".join(c * 2 for c in color)
        rgb = tuple(int(color[i:i + 2], 16) for i in (0, 2, 4))
        rects.append((
            float(rect.get("x", 0)), float(rect.get("y", 0)),
            float(rect.get("width")), float(rect.get("height")), rgb,
        ))

    if not rects:
        raise ValueError(f"{svg_path}: no <rect> elements with a solid fill")
    return view[2], view[3], rects


def render(size, view_w, view_h, rects):
    """Return RGBA bytes at size x size using exact rectangle coverage."""
    scale_x, scale_y = view_w / size, view_h / size
    pixel_area = scale_x * scale_y
    out = bytearray(size * size * 4)
    for py in range(size):
        sy0, sy1 = py * scale_y, (py + 1) * scale_y
        for px in range(size):
            sx0, sx1 = px * scale_x, (px + 1) * scale_x
            r = g = b = 0.0
            for x, y, w, h, (cr, cg, cb) in rects:
                ix = min(x + w, sx1) - max(x, sx0)
                if ix <= 0:
                    continue
                iy = min(y + h, sy1) - max(y, sy0)
                if iy <= 0:
                    continue
                area = ix * iy
                r += cr * area
                g += cg * area
                b += cb * area
            i = (py * size + px) * 4
            out[i] = int(r / pixel_area + 0.5)
            out[i + 1] = int(g / pixel_area + 0.5)
            out[i + 2] = int(b / pixel_area + 0.5)
            out[i + 3] = 255
    return bytes(out)


def png_bytes(size, rgba):
    raw = bytearray()
    for y in range(size):
        raw.append(0)  # filter type 0 (None)
        raw += rgba[y * size * 4:(y + 1) * size * 4]

    def chunk(typ, data):
        return (struct.pack(">I", len(data)) + typ + data
                + struct.pack(">I", zlib.crc32(typ + data) & 0xFFFFFFFF))

    return (b"\x89PNG\r\n\x1a\n"
            + chunk(b"IHDR", struct.pack(">IIBBBBB", size, size, 8, 6, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(bytes(raw), 9))
            + chunk(b"IEND", b""))


def bmp_bytes(size, rgba):
    """32-bit bottom-up DIB plus an all-zero AND mask (alpha carries shape)."""
    header = struct.pack("<IiiHHIIiiII", 40, size, size * 2, 1, 32, 0,
                         size * size * 4, 0, 0, 0, 0)
    xor = bytearray()
    for y in range(size - 1, -1, -1):
        row = rgba[y * size * 4:(y + 1) * size * 4]
        for x in range(size):
            r, g, b, a = row[x * 4:x * 4 + 4]
            xor += bytes((b, g, r, a))
    mask_stride = ((size + 31) // 32) * 4
    return header + bytes(xor) + b"\x00" * (mask_stride * size)


def build_ico(svg_path, ico_path):
    view_w, view_h, rects = parse_rects(svg_path)

    # PNG entries for the large sizes, 32-bit BMP for the small ones: both are
    # understood by every Windows version that runs Neurus.
    images = {}
    for size in SIZES:
        rgba = render(size, view_w, view_h, rects)
        images[size] = png_bytes(size, rgba) if size >= 128 else bmp_bytes(size, rgba)

    directory = bytearray()
    blobs = bytearray()
    offset = 6 + 16 * len(SIZES)
    for size in SIZES:
        data = images[size]
        dim = 0 if size >= 256 else size
        directory += struct.pack("<BBBBHHII", dim, dim, 0, 0, 1, 32,
                                 len(data), offset)
        blobs += data
        offset += len(data)

    ico_path.write_bytes(struct.pack("<HHH", 0, 1, len(SIZES)) + bytes(directory) + bytes(blobs))

    preview = render(256, view_w, view_h, rects)
    colours = len({preview[i:i + 4] for i in range(0, len(preview), 4)})
    print(f"{svg_path} -> {ico_path} ({ico_path.stat().st_size} bytes)")
    print(f"  {len(rects)} rects, {len(SIZES)} sizes, 256px preview has {colours} colours")
    if colours <= 1:
        raise SystemExit("icon is a single flat colour - refusing to keep it")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("svg", nargs="?", type=Path, default=DEFAULT_SVG)
    parser.add_argument("ico", nargs="?", type=Path, default=DEFAULT_ICO)
    args = parser.parse_args()
    build_ico(args.svg, args.ico)


if __name__ == "__main__":
    main()
