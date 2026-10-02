"""Colour: k-means palettes in CIE Lab, and ramps along named paths.

Palettes are k-means (OpenCV) over the subject's (or a region's) pixels in
Lab; each entry is a centre and the share of pixels nearest it, sorted by
lightness. Ramps sample the median Lab inside discs along a path (the spine
from head to tail, each wing from root to tip), inside the mask only.
Differences are CIEDE2000 (skimage).
"""

import cv2
import numpy as np
from scipy.optimize import linear_sum_assignment
from skimage.color import deltaE_ciede2000

from common import lab_to_bgr8, put_label


def palette(lab, sel, k=6, seed=1, max_px=60000):
    px = lab[sel].reshape(-1, 3).astype(np.float32)
    if len(px) < k * 10:
        return []
    rng = np.random.default_rng(seed)
    if len(px) > max_px:
        px = px[rng.choice(len(px), max_px, replace=False)]
    cv2.setRNGSeed(seed)
    crit = (cv2.TERM_CRITERIA_EPS + cv2.TERM_CRITERIA_MAX_ITER, 50, 0.05)
    _, labels, centres = cv2.kmeans(px, k, None, crit, 4, cv2.KMEANS_PP_CENTERS)
    share = np.bincount(labels.ravel(), minlength=k) / len(labels)
    order = np.argsort(centres[:, 0])
    return [{"lab": centres[i], "share": float(share[i])} for i in order]


def mean_lab(lab, sel):
    return lab[sel].reshape(-1, 3).mean(0) if sel.any() else np.array([np.nan] * 3)


def ramp(lab, mask, path_xy, n=12, radius=6):
    path_xy = np.asarray(path_xy, float)
    if len(path_xy) < 2:
        return []
    d = np.r_[0, np.cumsum(np.linalg.norm(np.diff(path_xy, axis=0), axis=1))]
    out = []
    h, w = mask.shape
    yy, xx = np.ogrid[:h, :w]
    for t in np.linspace(0, 1, n):
        j = int(np.argmin(np.abs(d - t * d[-1])))
        x, y = path_xy[j]
        x0, x1 = int(max(0, x - radius)), int(min(w, x + radius + 1))
        y0, y1 = int(max(0, y - radius)), int(min(h, y + radius + 1))
        disc = ((xx[:, x0:x1] - x) ** 2 + (yy[y0:y1] - y) ** 2 <= radius * radius) & (mask[y0:y1, x0:x1] > 0)
        if disc.sum() < 3:
            out.append({"t": t, "lab": [np.nan] * 3})
            continue
        out.append({"t": t, "lab": np.median(lab[y0:y1, x0:x1][disc], axis=0), "at_px": [x, y]})
    return out


def palette_delta(p_ref, p_ours):
    """Share-weighted CIEDE2000 between two palettes, centres matched one to
    one by the Hungarian method on ΔE."""
    if not p_ref or not p_ours:
        return None
    a = np.array([e["lab"] for e in p_ref], float)
    b = np.array([e["lab"] for e in p_ours], float)
    cost = deltaE_ciede2000(a[:, None, :].repeat(len(b), 1), b[None, :, :].repeat(len(a), 0))
    r, c = linear_sum_assignment(cost)
    wts = np.array([p_ref[i]["share"] for i in r])
    return {"weighted_dE00": float((cost[r, c] * wts).sum() / wts.sum()),
            "pairs": [{"ref": a[i], "ours": b[j], "dE00": cost[i, j]} for i, j in zip(r, c)]}


def de00(l1, l2):
    return float(deltaE_ciede2000(np.asarray(l1, float)[None], np.asarray(l2, float)[None])[0])


def draw_palettes(palettes, ramps, width=900):
    """A swatch sheet: one row per region palette (widths by share), then
    one row per ramp."""
    rows = len(palettes) + len(ramps)
    rh = 46
    img = np.full((rows * rh + 20, width, 3), 30, np.uint8)
    y = 10
    x0 = 170
    for name, pal in palettes.items():
        put_label(img, name, (8, y + 28), 0.55)
        x = x0
        for e in pal:
            wpx = max(2, int(round(e["share"] * (width - x0 - 10))))
            col = lab_to_bgr8(e["lab"])[0].tolist()
            cv2.rectangle(img, (x, y), (x + wpx, y + rh - 8), col, -1)
            if wpx > 60:
                put_label(img, f"L{e['lab'][0]:.0f} a{e['lab'][1]:.0f} b{e['lab'][2]:.0f}", (x + 3, y + 24), 0.38)
            x += wpx
        y += rh
    for name, rp in ramps.items():
        put_label(img, "ramp " + name, (8, y + 28), 0.5)
        n = max(1, len(rp))
        wpx = (width - x0 - 10) / n
        for i, e in enumerate(rp):
            if np.isnan(e["lab"][0]):
                continue
            col = lab_to_bgr8(e["lab"])[0].tolist()
            cv2.rectangle(img, (int(x0 + i * wpx), y), (int(x0 + (i + 1) * wpx) - 1, y + rh - 8), col, -1)
        y += rh
    return img
