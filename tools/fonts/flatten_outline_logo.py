# Flattens an angled photo of the outlined "Mirage" logo (yellow outline letters)
# and fills it into a solid-letter mask for trace_wordmark.py:
#   1. the yellow outline is found by its yellowness, min(R, G) - B;
#   2. four lines are fitted through the middle of the stroke: the baseline (bottoms
#      of M, i and r), the cap line (top of the M), the left edge of the M and the
#      right edge of the g's stem;
#   3. a perspective transform maps their corners onto a rectangle with the
#      proportions of the same lines on the front-on DSK decal (261 x 52 px there);
#   4. inside the outline the letters are filled (counters stay open) and the fill
#      is pulled back to the middle of the stroke, which is the letter edge.
# Needs numpy and opencv-python-headless.
# usage: flatten_outline_logo.py <mirage.webp> <out-mask.png> [scale=8]
# The output is white letters on black, ready for:
#   trace_wordmark.py <out-mask.png> mirage_wordmark.svg preview.png <box> FIELD_GRAY=1
import sys

import cv2
import numpy as np

REF_W, REF_H = 261.0, 52.0  # M left edge -> g stem right edge, cap line -> baseline (DSK decal)
MARGIN_L, MARGIN_T = 12.0, 8.0     # in reference units, around that rectangle
OUT_W, OUT_H = 345.0, 95.0         # reference units: the whole word plus margins


def yellowness(img):
    b, g, r = (img[..., i].astype(float) for i in range(3))
    return np.minimum(r, g) - b


def stroke_mask(img):
    b, g, r = (img[..., i].astype(int) for i in range(3))
    return ((np.minimum(r, g) - b) > 70) & (g > 150)


def mid_points_rows(mask, y0, y1, x0, x1, side):
    """Middle of the first (side='left') or last (side='right') stroke run on each row."""
    pts = []
    for y in range(y0, y1):
        xs = np.where(mask[y, x0:x1])[0]
        if not len(xs):
            continue
        runs = np.split(xs, np.where(np.diff(xs) > 1)[0] + 1)
        run = runs[0] if side == "left" else runs[-1]
        pts.append((x0 + (run[0] + run[-1]) / 2.0, y))
    return np.array(pts, float)


def mid_points_cols(mask, x0, x1, y0, y1, side):
    """Middle of the first (side='top') or last (side='bottom') stroke run in each column."""
    pts = []
    for x in range(x0, x1):
        ys = np.where(mask[y0:y1, x])[0]
        if not len(ys):
            continue
        runs = np.split(ys, np.where(np.diff(ys) > 1)[0] + 1)
        run = runs[0] if side == "top" else runs[-1]
        pts.append((x, y0 + (run[0] + run[-1]) / 2.0))
    return np.array(pts, float)


def fit_line(pts):
    """Total-least-squares line through pts, as (point, unit direction)."""
    c = pts.mean(axis=0)
    _, _, vt = np.linalg.svd(pts - c)
    return c, vt[0]


def intersect(l1, l2):
    (p, d), (q, e) = l1, l2
    a = np.array([d, -e]).T
    t = np.linalg.solve(a, q - p)
    return p + t[0] * d


def main():
    src, out = sys.argv[1], sys.argv[2]
    scale = float(sys.argv[3]) if len(sys.argv) > 3 else 8.0
    img = cv2.imread(src, cv2.IMREAD_COLOR)
    if img is None:
        sys.exit(f"cannot read {src}")
    mask = stroke_mask(img)

    # Sample ranges for the 1280x720 photo the committed wordmark came from.
    base = np.vstack([mid_points_cols(mask, 35, 108, 300, 345, "bottom"),    # M left stem
                      mid_points_cols(mask, 383, 444, 300, 345, "bottom"),   # M right stem
                      mid_points_cols(mask, 488, 537, 300, 345, "bottom"),   # i
                      mid_points_cols(mask, 571, 608, 300, 345, "bottom")])  # r stem
    cap = np.vstack([mid_points_cols(mask, 35, 156, 10, 60, "top"),           # M top left
                     mid_points_cols(mask, 335, 432, 30, 80, "top")])        # M top right
    left = mid_points_rows(mask, 40, 320, 0, 120, "left")                   # M left edge
    right = mid_points_rows(mask, 185, 340, 1040, 1099, "right")            # g stem right edge
    lb, lc, ll, lr = fit_line(base), fit_line(cap), fit_line(left), fit_line(right)
    tl, tr = intersect(ll, lc), intersect(lr, lc)
    bl, br = intersect(ll, lb), intersect(lr, lb)
    print("corners", np.round([tl, tr, br, bl], 1).tolist())

    s = scale
    dst = np.float32([[MARGIN_L * s, MARGIN_T * s], [(MARGIN_L + REF_W) * s, MARGIN_T * s],
                      [(MARGIN_L + REF_W) * s, (MARGIN_T + REF_H) * s],
                      [MARGIN_L * s, (MARGIN_T + REF_H) * s]])
    H = cv2.getPerspectiveTransform(np.float32([tl, tr, br, bl]), dst)
    size = (int(OUT_W * s), int(OUT_H * s))
    field = cv2.warpPerspective(yellowness(img), H, size, flags=cv2.INTER_CUBIC)
    stroke = (field > 70).astype(np.uint8)

    # Fill: regions enclosed by the outline at odd depth are letter bodies.
    contours, hier = cv2.findContours(stroke, cv2.RETR_TREE, cv2.CHAIN_APPROX_NONE)
    filled = np.zeros_like(stroke)
    def depth(i):
        d = 0
        while hier[0][i][3] >= 0:
            i = hier[0][i][3]
            d += 1
        return d
    # Stroke components are at even depth; the holes inside them at odd depth.
    # A hole at depth 1 (inside an outer outline) is a letter body; at depth 3
    # (inside a counter's outline) it is the counter.
    for i, c in enumerate(contours):
        if cv2.contourArea(c) < 4 * s * s:
            continue
        dd = depth(i)
        if dd % 2 == 0:
            cv2.drawContours(filled, contours, i, 1, thickness=cv2.FILLED)
        elif dd % 4 == 3:
            cv2.drawContours(filled, contours, i, 0, thickness=cv2.FILLED)
    # Pull back to the middle of the stroke: keep pixels farther than half the
    # stroke width from the outside.
    dist_in = cv2.distanceTransform(stroke, cv2.DIST_L2, 5)
    half = float(np.percentile(dist_in[stroke > 0], 95))  # about half the stroke width
    dist_out = cv2.distanceTransform(filled, cv2.DIST_L2, 5)
    letters = (dist_out > half).astype(np.uint8) * 255
    print(f"half stroke {half / s:.2f} ref px  output {size[0]}x{size[1]}")
    cv2.imwrite(out, letters)


if __name__ == "__main__":
    main()
