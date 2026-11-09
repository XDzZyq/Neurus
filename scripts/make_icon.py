"""Generate the platform application icons from the source SVG.

The icon lives in the repository as an SVG (embedded in the Qt resources and
used for the window/taskbar icon), an ICO (compiled into Neurus.exe by
res/ui/logo/neurus.rc) and an ICNS (packaged into Neurus.app by CMake on macOS).
Neither binary format can be derived at build time without adding an SVG
rasteriser to the toolchain, so both are committed and regenerated here whenever
the SVG changes:

    python scripts/make_icon.py

On macOS the .icns is what Finder, the Dock and Cmd-Tab read; the bundle's
CFBundleIconFile names it. Both files are written from the same rectangles, so
they always depict the same artwork.

The renderer understands the subset of SVG the logo uses: a viewBox, a <style>
block mapping class selectors to solid fills, and <rect> elements. Each output
pixel is an exact area-weighted average of the rectangles covering it, which
gives clean antialiasing without an external library.

The README and the documentation site show the same artwork. MkDocs only serves
files under `docs_dir`, so this script also refreshes the copy at
`docs/assets/neurus.svg` whenever it regenerates the default icon.
"""

import argparse
import math
import struct
import xml.etree.ElementTree as ET
import zlib
from pathlib import Path

# Sizes Windows asks for: taskbar, title bar, Alt-Tab, Explorer, large icons.
SIZES = [16, 24, 32, 48, 64, 128, 256]

# ICNS entries as (OSType, pixel size). icp4..icp6 are the legacy small slots;
# ic07..ic10 are 1x, ic11..ic14 the matching @2x slots. Several types share a
# size, so each distinct size is rendered once and reused.
ICNS_ENTRIES = [
    (b"icp4", 16),
    (b"icp5", 32),
    (b"icp6", 64),
    (b"ic07", 128),
    (b"ic08", 256),
    (b"ic09", 512),
    (b"ic10", 1024),
    (b"ic11", 32),
    (b"ic12", 64),
    (b"ic13", 256),
    (b"ic14", 512),
]

SVG_NS = "{http://www.w3.org/2000/svg}"
REPO_ROOT = Path(__file__).resolve().parent.parent
DEFAULT_SVG = REPO_ROOT / "res" / "ui" / "logo" / "neurus.svg"
DEFAULT_ICO = REPO_ROOT / "res" / "ui" / "logo" / "neurus.ico"
DEFAULT_ICNS = REPO_ROOT / "res" / "ui" / "logo" / "neurus.icns"
DOCS_SVG = REPO_ROOT / "docs" / "assets" / "neurus.svg"


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
    """Return RGBA bytes at size x size using exact rectangle coverage.

    Each rectangle only contributes to the pixel rows and columns it overlaps,
    so the cost follows the covered area rather than pixels x rectangles. The
    per-pixel accumulation still happens in document order, which keeps the
    result bit-identical to a brute-force scan of every pixel.
    """
    scale_x, scale_y = view_w / size, view_h / size
    pixel_area = scale_x * scale_y
    cells = size * size
    red = [0.0] * cells
    green = [0.0] * cells
    blue = [0.0] * cells
    for x, y, w, h, (cr, cg, cb) in rects:
        x1, y1 = x + w, y + h
        # One pixel of slack on each side: floor/ceil on the scaled edge can be
        # off by an ulp, and a pixel outside the true range accumulates nothing.
        px0 = max(0, math.floor(x / scale_x) - 1)
        px1 = min(size, math.ceil(x1 / scale_x) + 1)
        py0 = max(0, math.floor(y / scale_y) - 1)
        py1 = min(size, math.ceil(y1 / scale_y) + 1)
        if px0 >= px1 or py0 >= py1:
            continue
        spans = [min(x1, (px + 1) * scale_x) - max(x, px * scale_x)
                 for px in range(px0, px1)]
        for py in range(py0, py1):
            sy0, sy1 = py * scale_y, (py + 1) * scale_y
            iy = min(y1, sy1) - max(y, sy0)
            if iy <= 0:
                continue
            base = py * size + px0
            for k, ix in enumerate(spans):
                if ix <= 0:
                    continue
                area = ix * iy
                i = base + k
                red[i] += cr * area
                green[i] += cg * area
                blue[i] += cb * area

    out = bytearray(cells * 4)
    for i in range(cells):
        j = i * 4
        out[j] = int(red[i] / pixel_area + 0.5)
        out[j + 1] = int(green[i] / pixel_area + 0.5)
        out[j + 2] = int(blue[i] / pixel_area + 0.5)
        out[j + 3] = 255
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

    # MkDocs only serves files under docs_dir, so the site keeps its own copy of
    # the source artwork; refresh it alongside the icon it belongs to.
    if svg_path.resolve() == DEFAULT_SVG.resolve():
        DOCS_SVG.parent.mkdir(parents=True, exist_ok=True)
        DOCS_SVG.write_bytes(svg_path.read_bytes())
        print(f"  synced {DOCS_SVG.relative_to(REPO_ROOT)}")

    preview = render(256, view_w, view_h, rects)
    colours = len({preview[i:i + 4] for i in range(0, len(preview), 4)})
    print(f"{svg_path} -> {ico_path} ({ico_path.stat().st_size} bytes)")
    print(f"  {len(rects)} rects, {len(SIZES)} sizes, 256px preview has {colours} colours")
    if colours <= 1:
        raise SystemExit("icon is a single flat colour - refusing to keep it")


def build_icns(svg_path, icns_path):
    """Write a macOS .icns with PNG entries for every slot in ICNS_ENTRIES."""
    view_w, view_h, rects = parse_rects(svg_path)

    # icp5/ic11 (32), icp6/ic12 (64), ic08/ic13 (256) and ic09/ic14 (512) share
    # a pixel size, so render each distinct size exactly once.
    blobs = {}
    for _, size in ICNS_ENTRIES:
        if size not in blobs:
            blobs[size] = png_bytes(size, render(size, view_w, view_h, rects))

    body = bytearray()
    for ostype, size in ICNS_ENTRIES:
        data = blobs[size]
        body += ostype + struct.pack(">I", 8 + len(data)) + data

    # "icns" magic, total file length, then the entries (each length includes
    # its own 8-byte type/length header).
    icns_path.write_bytes(b"icns" + struct.pack(">I", 8 + len(body)) + bytes(body))
    print(f"{svg_path} -> {icns_path} ({icns_path.stat().st_size} bytes)")
    print(f"  {len(ICNS_ENTRIES)} entries, {len(blobs)} distinct sizes")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("svg", nargs="?", type=Path, default=DEFAULT_SVG)
    parser.add_argument("ico", nargs="?", type=Path, default=DEFAULT_ICO)
    parser.add_argument("icns", nargs="?", type=Path, default=DEFAULT_ICNS)
    args = parser.parse_args()
    build_ico(args.svg, args.ico)
    build_icns(args.svg, args.icns)


if __name__ == "__main__":
    main()
