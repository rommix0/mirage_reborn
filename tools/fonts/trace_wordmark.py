# Traces the yellow "Mirage" wordmark from a photo of a Mirage DSK panel into
# app/resources/mirage_wordmark.svg, by edges rather than a blurred threshold:
#   1. crop and upscale; the "yellowness" min(R, G) - B separates the yellow
#      ink from the dark panel and from the white rule the "g" crosses;
#   2. total-variation denoising flattens JPEG noise but keeps edges sharp;
#   3. marching squares finds each letter edge with sub-pixel accuracy at the
#      half-way level (where the edge gradient peaks);
#   4. each contour is split at its corners; nearly straight runs become exact
#      lines (snapped horizontal or vertical when within a few degrees) and the
#      rest are smoothed and written as cubic Beziers.
# Needs numpy, Pillow, scipy and scikit-image (pip install numpy pillow scipy scikit-image).
# The committed SVG comes from an angled photo of the outlined logo, flattened and
# filled first (1280x720 mirage.webp):
#   flatten_outline_logo.py mirage.webp mask.png 8
#   trace_wordmark.py mask.png mirage_wordmark.svg preview.png FIELD_GRAY=1 UPSCALE=1 TV_WEIGHT=0
#       BLUR=2 STEP=1 CORNER_SPAN=8 CORNER_DEG=40 STRAIGHT_TOL=1.2 SNAP_DEG=4 SMOOTH_SIGMA=3
#       CURVE_STEP=8 MIN_AREA=400 PREVIEW_SCALE=0.5
# The defaults suit a front-on photo of the solid decal (1600x1140 4.jpg).
# usage: trace_wordmark.py <photo.jpg> <out.svg> <preview.png> [x0 y0 x1 y1] [NAME=value ...]
#   (NAME is any of the settings below, e.g. TV_WEIGHT=0.05 SMOOTH_SIGMA=0.6)
import sys

import numpy as np
from PIL import Image, ImageDraw
from scipy.ndimage import gaussian_filter, gaussian_filter1d
from skimage import measure, restoration

UPSCALE = 4          # work on a 4x bicubic upscale of the crop
TV_WEIGHT = 0.20     # total-variation denoising strength (0..1 range image)
STEP = 0.25          # contour resampling step, photo pixels
CORNER_DEG = 48      # turning angle (over CORNER_SPAN each side) that makes a corner
CORNER_SPAN = 1.25   # photo pixels
STRAIGHT_TOL = 0.40  # max deviation from the chord for a run to become a line, photo px
SNAP_DEG = 5         # lines this close to horizontal or vertical are made exact
SMOOTH_SIGMA = 1.8   # smoothing of curved runs along their length, photo px
CURVE_STEP = 2.5     # spacing of Bezier knots on curved runs, photo px
MIN_AREA = 4.0       # contours smaller than this (photo px^2) are noise
FIELD_GRAY = 0       # 1: the input is a white-letters-on-black mask (flatten_outline_logo.py)
BLUR = 0.0           # Gaussian blur of the field before edge finding, input px (anti-aliases a mask)
PREVIEW_SCALE = 8    # preview magnification


def cross2(a, b):
    """z of the 2-D cross product (numpy 2 dropped 2-D np.cross)."""
    return a[..., 0] * b[..., 1] - a[..., 1] * b[..., 0]


def load_field(photo, box):
    up = int(UPSCALE)
    img = Image.open(photo).convert("RGB").crop(box)
    if up != 1:
        img = img.resize((img.width * up, img.height * up), Image.BICUBIC)
    a = np.asarray(img).astype(float)
    if FIELD_GRAY:
        y = a.mean(axis=2) / 255.0
    else:
        y = np.minimum(a[..., 0], a[..., 1]) - a[..., 2]
        lo, hi = np.percentile(y, 5), np.percentile(y, 97)
        y = np.clip((y - lo) / (hi - lo), 0.0, 1.0)
    if BLUR > 0:
        y = gaussian_filter(y, BLUR * up)
    if TV_WEIGHT > 0.002:
        y = restoration.denoise_tv_chambolle(y, weight=TV_WEIGHT)
    return y


def resample(pts, step):
    closed = np.vstack([pts, pts[:1]])
    seg = np.linalg.norm(np.diff(closed, axis=0), axis=1)
    s = np.concatenate([[0], np.cumsum(seg)])
    n = max(int(s[-1] / step), 8)
    t = np.linspace(0, s[-1], n, endpoint=False)
    return np.column_stack([np.interp(t, s, closed[:, 0]), np.interp(t, s, closed[:, 1])])


def find_corners(p):
    n, k = len(p), max(2, int(round(CORNER_SPAN / STEP)))
    fwd = np.roll(p, -k, axis=0) - p
    back = p - np.roll(p, k, axis=0)
    ang = np.degrees(np.arctan2(cross2(back, fwd), np.einsum("ij,ij->i", back, fwd)))
    turn = np.abs(ang)
    corners = []
    for i in range(n):
        if turn[i] < CORNER_DEG:
            continue
        window = [turn[(i + j) % n] for j in range(-k, k + 1)]
        if turn[i] >= max(window) and all(abs(i - c) > k for c in corners):
            corners.append(i)
    return sorted(corners)


def runs_between(p, corners):
    n = len(p)
    if not corners:
        return [np.vstack([p, p[:1]])], True
    out = []
    for a, b in zip(corners, corners[1:] + [corners[0] + n]):
        idx = [i % n for i in range(a, b + 1)]
        out.append(p[idx])
    return out, False


def is_straight(run):
    a, b = run[0], run[-1]
    d = b - a
    L = np.linalg.norm(d)
    if L < 1e-6:
        return False
    dist = np.abs(cross2(d, run - a)) / L
    return dist.max() <= STRAIGHT_TOL


def snap_lines(corner_pts, straight):
    # Make near-axis lines exact by moving their two corner points together.
    m = len(corner_pts)
    for i in range(m):
        if not straight[i]:
            continue
        a, b = corner_pts[i], corner_pts[(i + 1) % m]
        d = b - a
        ang = abs(np.degrees(np.arctan2(d[1], d[0]))) % 180
        if min(ang, 180 - ang) <= SNAP_DEG:            # horizontal
            yv = (a[1] + b[1]) / 2
            a[1] = b[1] = yv
        elif abs(ang - 90) <= SNAP_DEG:                # vertical
            xv = (a[0] + b[0]) / 2
            a[0] = b[0] = xv


def smooth_run(run):
    if len(run) < 5:
        return run
    sm = np.column_stack([gaussian_filter1d(run[:, i], SMOOTH_SIGMA / STEP, mode="nearest")
                          for i in range(2)])
    # Keep the ends on the corners.
    w = np.minimum(np.arange(len(run)), np.arange(len(run))[::-1]) / max(1, SMOOTH_SIGMA / STEP)
    w = np.clip(w, 0, 1)[:, None]
    return run * (1 - w) + sm * w


def catmull_rom(points, closed):
    # Cubic Bezier control points through `points` (Catmull-Rom, tension 0.5).
    segs = []
    n = len(points)
    rng = range(n) if closed else range(n - 1)
    for i in rng:
        p0 = points[(i - 1) % n] if (closed or i > 0) else points[i]
        p1, p2 = points[i], points[(i + 1) % n]
        p3 = points[(i + 2) % n] if (closed or i + 2 < n) else points[(i + 1) % n]
        segs.append((p1 + (p2 - p0) / 6, p2 - (p3 - p1) / 6, p2))
    return segs


def knots(run):
    L = np.sum(np.linalg.norm(np.diff(run, axis=0), axis=1))
    k = max(2, int(round(L / CURVE_STEP)))
    idx = np.linspace(0, len(run) - 1, k + 1).round().astype(int)
    return run[idx]


def contour_to_path(p):
    """Returns (svg path data, flattened polygon) for one closed contour (x, y)."""
    p = resample(p, STEP)
    corners = find_corners(p)
    runs, smooth_loop = runs_between(p, corners)
    fmt = lambda q: f"{q[0]:.2f},{q[1]:.2f}"
    poly = []
    if smooth_loop:
        pts = knots(smooth_run(runs[0]))[:-1]
        segs = catmull_rom(pts, closed=True)
        d = [f"M{fmt(pts[0])}"] + [f"C{fmt(c1)} {fmt(c2)} {fmt(e)}" for c1, c2, e in segs] + ["Z"]
        start = pts[0]
        for c1, c2, e in segs:
            poly += bezier_points(start, c1, c2, e)
            start = e
        return "".join(d), np.array(poly)
    straight = [is_straight(r) for r in runs]
    cpts = [p[c].copy() for c in corners]
    snap_lines(cpts, straight)
    d = [f"M{fmt(cpts[0])}"]
    start = cpts[0]
    m = len(cpts)
    for i, run in enumerate(runs):
        a, b = cpts[i], cpts[(i + 1) % m]
        if straight[i]:
            d.append(f"L{fmt(b)}")
            poly += [a, b]
        else:
            r = smooth_run(run.copy())
            r[0], r[-1] = a, b
            pts = knots(r)
            for c1, c2, e in catmull_rom(pts, closed=False):
                d.append(f"C{fmt(c1)} {fmt(c2)} {fmt(e)}")
                poly += bezier_points(start, c1, c2, e)
                start = e
        start = b
    d.append("Z")
    return "".join(d), np.array(poly)


def bezier_points(p0, c1, c2, p3, n=8):
    t = np.linspace(0, 1, n)[:, None]
    return list((1 - t) ** 3 * p0 + 3 * (1 - t) ** 2 * t * c1 + 3 * (1 - t) * t ** 2 * c2 + t ** 3 * p3)


def main():
    # NAME=value arguments override the settings above (e.g. TV_WEIGHT=0.05).
    args = []
    for a in sys.argv[1:]:
        if "=" in a and a.split("=", 1)[0].isupper():
            name, value = a.split("=", 1)
            globals()[name] = float(value)
        else:
            args.append(a)
    photo, out, preview = args[0:3]
    if len(args) > 6:
        box = tuple(int(v) for v in args[3:7])
    elif FIELD_GRAY:
        box = (0, 0) + Image.open(photo).size
    else:
        box = (680, 366, 1008, 448)
    field = load_field(photo, box)
    contours = measure.find_contours(field, 0.5)
    paths, polys = [], []
    for c in contours:
        if np.linalg.norm(c[0] - c[-1]) > 1e-6:
            continue  # open (touches the crop edge)
        xy = np.column_stack([c[:, 1], c[:, 0]]) / int(UPSCALE)  # input px, relative to the crop
        area = 0.5 * abs(np.dot(xy[:, 0], np.roll(xy[:, 1], 1)) - np.dot(xy[:, 1], np.roll(xy[:, 0], 1)))
        if area < MIN_AREA:
            continue
        d, poly = contour_to_path(xy[:-1])
        paths.append(d)
        polys.append(poly)
    allp = np.vstack(polys)
    x0, y0 = allp.min(axis=0)
    x1, y1 = allp.max(axis=0)
    w, h = x1 - x0, y1 - y0
    m_left = allp[allp[:, 0] < x0 + 0.06 * w]
    base = m_left[:, 1].max()  # bottom of the "M"
    print(f"contours {len(paths)}  bbox {w:.1f} x {h:.1f} px  aspect {w / h:.3f}  "
          f"baseline fraction {(base - y0) / h:.3f}")
    with open(out, "w", newline="\n") as fh:
        fh.write('<?xml version="1.0" encoding="UTF-8"?>\n')
        fh.write("<!-- The \"Mirage\" wordmark traced by edge from a photo of a Mirage DSK panel; "
                 "tools/fonts/trace_wordmark.py. -->\n")
        fh.write(f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="{x0:.2f} {y0:.2f} {w:.2f} {h:.2f}">\n')
        fh.write('  <path fill="#000000" fill-rule="evenodd" d="' + "".join(paths) + '"/>\n</svg>\n')
    # Preview: the traced outline filled (even-odd) at 8x, under the photo crop at 8x.
    S = PREVIEW_SCALE
    mask = Image.new("1", (int(w * S) + 4, int(h * S) + 4), 0)
    for poly in polys:
        layer = Image.new("1", mask.size, 0)
        ImageDraw.Draw(layer).polygon([((x - x0) * S + 2, (y - y0) * S + 2) for x, y in poly], fill=1)
        mask = Image.fromarray(np.logical_xor(np.asarray(mask), np.asarray(layer)))
    crop = Image.open(photo).convert("RGB").crop(
        (int(box[0] + x0), int(box[1] + y0), int(box[0] + x1) + 1, int(box[1] + y1) + 1))
    crop = crop.resize(mask.size, Image.BICUBIC)
    sheet = Image.new("RGB", (mask.width, mask.height * 2 + 10), (128, 128, 128))
    sheet.paste(crop, (0, 0))
    ink = Image.new("RGB", mask.size, (242, 194, 46))
    bg = Image.new("RGB", mask.size, (36, 36, 40))
    sheet.paste(Image.composite(ink, bg, mask.convert("L")), (0, mask.height + 10))
    sheet.save(preview)


if __name__ == "__main__":
    main()
