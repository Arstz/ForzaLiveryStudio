#!/usr/bin/env python3
"""Regenerate the editor's XPM icons from their source PNGs.

The GUI loads its toolbar/menu/cursor art from ``assets/*.xpm`` at runtime
(Qt reads the XPM string data). This script rebuilds those XPMs from the matching
``*.png`` files so you can edit the PNGs and regenerate in one step.

Conventions matched (so diffs stay clean):
  * native PNG size, ``static const char *<Base>_xpm[]``
  * alpha thresholded to a single transparent ``None`` colour (XPM has no partial
    alpha), opaque pixels keep their exact RGB, uppercase hex
  * chars-per-pixel = 1 while the colour count fits (<=92), else 2
  * CRLF line endings

Usage (from anywhere - paths resolve next to this script):
    python gen_xpm.py            # rebuild every PNG that already has an .xpm
    python gen_xpm.py --all      # also create .xpm for PNGs that don't have one yet
    python gen_xpm.py --check    # report what would change; exit 1 if anything is stale
    python gen_xpm.py <dir>      # use a different assets directory

Requires Pillow:  python -m pip install pillow
"""
import os
import string
import sys

try:
    from PIL import Image
except ImportError:
    sys.exit("Pillow is required: run  python -m pip install pillow")

# Fully transparent below this alpha; opaque (keeps its RGB) at or above it.
ALPHA_THRESHOLD = 128
# Readable keys first (a-z A-Z 0-9), then punctuation minus the XPM-illegal " and \.
ALPHABET = (string.ascii_lowercase + string.ascii_uppercase + string.digits +
            "".join(c for c in string.punctuation if c not in '"\\'))


def keys_for(count, cpp):
    if cpp == 1:
        return list(ALPHABET[:count])
    out = []
    for a in ALPHABET:
        for b in ALPHABET:
            out.append(a + b)
            if len(out) == count:
                return out
    raise RuntimeError("too many colours for 2 chars per pixel")


def render_xpm(png_path, base):
    im = Image.open(png_path).convert("RGBA")
    w, h = im.size
    px = im.load()

    order = []            # colour value in first-appearance (raster) order
    index = {}            # value -> position in order
    grid = [[None] * w for _ in range(h)]
    for y in range(h):
        for x in range(w):
            r, g, b, a = px[x, y]
            value = "None" if a < ALPHA_THRESHOLD else "#%02X%02X%02X" % (r, g, b)
            if value not in index:
                index[value] = len(order)
                order.append(value)
            grid[y][x] = value

    ncolors = len(order)
    cpp = 1 if ncolors <= len(ALPHABET) else 2
    keys = keys_for(ncolors, cpp)
    key_of = {value: keys[i] for i, value in enumerate(order)}

    lines = ['"%d %d %d %d"' % (w, h, ncolors, cpp)]
    for value in order:
        lines.append('"%s c %s"' % (key_of[value], value))
    for y in range(h):
        lines.append('"' + "".join(key_of[grid[y][x]] for x in range(w)) + '"')

    body = ",\r\n".join(lines)
    text = ("/* XPM */\r\n"
            "static const char *%s_xpm[] = {\r\n%s\r\n};\r\n" % (base, body))
    return (w, h, ncolors, cpp), text.encode("ascii")


def main(argv):
    generate_all = "--all" in argv
    check_only = "--check" in argv
    positional = [a for a in argv if not a.startswith("-")]
    here = os.path.dirname(os.path.abspath(__file__))
    repo_root = os.path.dirname(here)
    assets = positional[0] if positional else os.path.join(repo_root, "assets")
    if not os.path.isdir(assets):
        sys.exit("assets directory not found: %s" % assets)

    pngs = sorted(n[:-4] for n in os.listdir(assets) if n.lower().endswith(".png"))
    if not pngs:
        sys.exit("no PNG files found in %s" % assets)

    stale = 0
    written = 0
    skipped = 0
    for base in pngs:
        xpm_path = os.path.join(assets, base + ".xpm")
        if not generate_all and not os.path.exists(xpm_path):
            skipped += 1
            print("  skip (png only, use --all): %s" % base)
            continue
        (w, h, n, cpp), data = render_xpm(os.path.join(assets, base + ".png"), base)
        current = open(xpm_path, "rb").read() if os.path.exists(xpm_path) else None
        if current == data:
            continue
        stale += 1
        if check_only:
            print("  STALE: %s (%dx%d, %d colours)" % (base, w, h, n))
            continue
        with open(xpm_path, "wb") as f:
            f.write(data)
        written += 1
        print("  %-22s %dx%d  colours=%d cpp=%d" % (base, w, h, n, cpp))

    if check_only:
        print("%d xpm file(s) out of date" % stale)
        return 1 if stale else 0
    print("wrote %d xpm file(s); %d already up to date; %d png-only skipped"
          % (written, len(pngs) - written - skipped, skipped))
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
