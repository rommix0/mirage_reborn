# Traces the yellow "Mirage" wordmark from a photo of a Mirage DSK panel into
# app/resources/mirage_wordmark.svg: crop, upscale, keep the yellow pixels (which
# drops the white rule the "g" crosses), smooth, then potrace.
# Needs numpy, Pillow and potracer (pip install numpy pillow potracer).
# The committed SVG was made from a 1600x1140 photo with the defaults except
# blur 7: trace_wordmark.py 4.jpg mirage_wordmark.svg mask.png 680 366 1008 448 8 7 70
# usage: trace_wordmark.py <photo.jpg> <out.svg> <preview.png> [x0 y0 x1 y1] [scale] [blur] [threshold]
import sys
import numpy as np
from PIL import Image, ImageFilter
import potrace

photo, out, preview = sys.argv[1:4]
x0, y0, x1, y1 = (int(v) for v in (sys.argv[4:8] if len(sys.argv) > 7 else (680, 366, 1008, 448)))
S = int(sys.argv[8]) if len(sys.argv) > 8 else 8
blur = float(sys.argv[9]) if len(sys.argv) > 9 else 3.0
thr = float(sys.argv[10]) if len(sys.argv) > 10 else 70
img = Image.open(photo).convert("RGB").crop((x0, y0, x1, y1))
big = img.resize((img.width * S, img.height * S), Image.BICUBIC).filter(ImageFilter.GaussianBlur(blur))
a = np.asarray(big).astype(float)
yellow = np.minimum(a[..., 0], a[..., 1]) - a[..., 2]  # high for yellow, ~0 for white/black
mask = yellow > thr
bm = potrace.Bitmap(~mask)  # potracer takes the False pixels as ink
plist = bm.trace(turdsize=200 * S, turnpolicy=potrace.POTRACE_TURNPOLICY_MINORITY,
                 alphamax=1.0, opticurve=True, opttolerance=0.2)

def f(v):  # back to photo pixels
    return f"{v / S:.2f}"

d = []
for curve in plist:
    sp = curve.start_point
    d.append(f"M{f(sp.x)},{f(sp.y)}")
    for seg in curve.segments:
        if seg.is_corner:
            d.append(f"L{f(seg.c.x)},{f(seg.c.y)}L{f(seg.end_point.x)},{f(seg.end_point.y)}")
        else:
            d.append(f"C{f(seg.c1.x)},{f(seg.c1.y)} {f(seg.c2.x)},{f(seg.c2.y)} {f(seg.end_point.x)},{f(seg.end_point.y)}")
    d.append("Z")
ys, xs = np.where(mask)
bx0, by0, bx1, by1 = xs.min() / S, ys.min() / S, (xs.max() + 1) / S, (ys.max() + 1) / S
# Baseline: the bottom of the "M" (the leftmost letter's lowest row).
mcols = xs < xs.min() + 20 * S
base = (ys[mcols].max() + 1) / S
w, h = bx1 - bx0, by1 - by0
print(f"paths {len(plist)}  bbox {w:.1f} x {h:.1f} px  aspect {w / h:.3f}  baseline fraction {(base - by0) / h:.3f}")
with open(out, "w", newline="\n") as fh:
    fh.write('<?xml version="1.0" encoding="UTF-8"?>\n')
    fh.write("<!-- The \"Mirage\" wordmark traced (potrace) from a photo of a Mirage DSK panel; "
             "tools/fonts/trace_wordmark.py. -->\n")
    fh.write(f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="{bx0:.2f} {by0:.2f} {w:.2f} {h:.2f}">\n')
    fh.write('  <path fill="#000000" fill-rule="evenodd" d="' + "".join(d) + '"/>\n</svg>\n')
Image.fromarray((mask * 255).astype(np.uint8)).resize((mask.shape[1] // 2, mask.shape[0] // 2)).save(preview)
