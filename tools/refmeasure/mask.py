"""The subject mask: the ship against space, a planet and its limb.

Classic tools only. Colour alone fails here: the ship's shadowed body is as
black as space (L* about 4 against 1 to 4). What does separate it is local
detail: the ship is busy at every lightness, while space and the planet are
smooth. Each pixel gets four numbers: detail (the mean absolute difference
of L* from its blur, pooled; stars taken out), warmth a* + b* (the planet
is warm), pinkness a* (the planet's lit face is pinker than the ship's
blue-grey) and lightness (the limb is very light). Thresholds on them seed
GrabCut, definite where the evidence is strong and probable elsewhere, and
GrabCut runs on an image whose three channels are detail, warmth and
lightness, so its models learn that evidence instead of colour. Morphology
then opens and closes the result, fills small holes, and drops specks.

A hint JSON can help where this fails:
  {"box": [x0, y0, x1, y1],          # everything outside is background
   "fg": [[x, y, r], ...],           # discs that are certainly ship
   "bg": [[x, y, r], ...],           # discs that are certainly not
   "head": [x, y], "tail": [x, y]}   # used by the skeleton, not here
in the cropped picture's pixels.
"""

import cv2
import numpy as np
from scipy import ndimage as ndi

from common import bgr_to_lab

# The class thresholds, in CIE Lab (L 0..100). Chosen on the Starship 1
# reference; recorded in the sheet so a change is visible.
PARAMS = {
    "detail_sigma": 2.0,       # detail: |L - blur(L, this)|
    "detail_pool": 6.0,        # pooled over a Gaussian this wide
    "detail_bg": 0.12,         # below: certainly background (smooth)
    "detail_pr_fg": 0.35,      # above: probably ship
    "detail_fg": 1.2,          # above (and cool): certainly ship
    "limb_L_min": 75.0,        # lighter than this: the planet's limb
    "warm_bg_min": 12.0,       # a*+b* above this, and smooth: planet
    "warm_pr_bg_min": 8.0,     # sure ship is cooler than this
    "planet_a_min": 6.5,       # a* above this and detail below the next: lit planet
    "planet_detail_max": 1.8,
    "grabcut_iters": 6,
    "min_component_frac": 0.004,
    "max_hole_frac": 0.0015,
}


def subject_mask(bgr, hint=None, params=None):
    p = dict(PARAMS)
    if params:
        p.update(params)
    h, w = bgr.shape[:2]
    lab = bgr_to_lab(bgr)
    L = cv2.GaussianBlur(lab[..., 0].astype(np.float32), (0, 0), 3)
    warm = cv2.GaussianBlur((lab[..., 1] + lab[..., 2]).astype(np.float32), (0, 0), 3)
    # Local detail: the ship is busy at every lightness, space and the
    # planet are smooth. Mean absolute difference from a blur, pooled.
    g = lab[..., 0].astype(np.float32)
    detail = cv2.GaussianBlur(np.abs(g - cv2.GaussianBlur(g, (0, 0), p["detail_sigma"])), (0, 0), p["detail_pool"])
    # Small bright specks (stars) are busy too: take them out of the detail
    stars = (g - cv2.GaussianBlur(g, (0, 0), 4) > 6) & (cv2.GaussianBlur(g, (0, 0), 8) < 6)
    stars = cv2.dilate(stars.astype(np.uint8), np.ones((9, 9), np.uint8)) > 0
    detail[stars] = np.minimum(detail[stars], 0.1)
    logd = np.log10(detail + 1e-3)

    # GrabCut models colour; give it the evidence instead: an image whose
    # three channels are log detail, warmth and lightness
    feat = np.dstack([
        np.clip((logd + 1.5) / 2.5 * 255, 0, 255),
        np.clip(warm * 4 + 128, 0, 255),
        np.clip(L * 2.55, 0, 255),
    ]).astype(np.uint8)

    gc = np.full((h, w), cv2.GC_PR_BGD, np.uint8)
    pr_fg = (detail > p["detail_pr_fg"]) & (warm < p["warm_bg_min"]) & (L < p["limb_L_min"])
    sure_fg = (detail > p["detail_fg"]) & (warm < p["warm_pr_bg_min"]) & (L < p["limb_L_min"])
    sure_bg = (detail < p["detail_bg"]) | (L > p["limb_L_min"]) | ((warm > p["warm_bg_min"]) & (detail < p["detail_pr_fg"]))
    # The planet's lit, cratered face is busy too, but pinker (a*) than the
    # ship's blue-grey and less busy than the ship's lit parts
    a_s = cv2.GaussianBlur(lab[..., 1].astype(np.float32), (0, 0), 3)
    lit_planet = (a_s > p["planet_a_min"]) & (detail < p["planet_detail_max"])
    pr_fg &= ~lit_planet
    sure_fg &= ~lit_planet
    sure_bg |= cv2.erode(lit_planet.astype(np.uint8), np.ones((9, 9), np.uint8)) > 0
    gc[pr_fg] = cv2.GC_PR_FGD
    # Sure foreground only away from its own edges (an eroded core)
    core = cv2.erode(sure_fg.astype(np.uint8), np.ones((5, 5), np.uint8)) > 0
    gc[core] = cv2.GC_FGD
    gc[sure_bg & ~core] = cv2.GC_BGD
    border = np.zeros((h, w), bool)
    border[:3, :] = border[-3:, :] = True
    border[:, :3] = border[:, -3:] = True
    gc[border] = cv2.GC_BGD

    used_hint = {}
    if hint:
        if "box" in hint:
            x0, y0, x1, y1 = [int(v) for v in hint["box"]]
            outside = np.ones((h, w), bool)
            outside[y0:y1, x0:x1] = False
            gc[outside] = cv2.GC_BGD
            used_hint["box"] = hint["box"]
        for key, val in (("fg", cv2.GC_FGD), ("bg", cv2.GC_BGD)):
            if key in hint:
                m = np.zeros((h, w), np.uint8)
                for x, y, r in hint[key]:
                    cv2.circle(m, (int(x), int(y)), int(r), 1, -1)
                gc[m > 0] = val
                used_hint[key] = hint[key]

    seeds = {
        "sure_fg_frac": float((gc == cv2.GC_FGD).mean()),
        "sure_bg_frac": float((gc == cv2.GC_BGD).mean()),
        "probable_fg_frac": float((gc == cv2.GC_PR_FGD).mean()),
    }
    bgm = np.zeros((1, 65), np.float64)
    fgm = np.zeros((1, 65), np.float64)
    cv2.grabCut(feat, gc, None, bgm, fgm, p["grabcut_iters"], cv2.GC_INIT_WITH_MASK)
    m = ((gc == cv2.GC_FGD) | (gc == cv2.GC_PR_FGD))

    # Clean: open away hairlines of planet, close cracks, fill small holes,
    # keep the components big enough to be ship
    k3 = cv2.getStructuringElement(cv2.MORPH_ELLIPSE, (3, 3))
    k5 = cv2.getStructuringElement(cv2.MORPH_ELLIPSE, (5, 5))
    m = cv2.morphologyEx(m.astype(np.uint8), cv2.MORPH_OPEN, k3)
    m = cv2.morphologyEx(m, cv2.MORPH_CLOSE, k5) > 0
    holes = ndi.binary_fill_holes(m) & ~m
    lab_h, n_h = ndi.label(holes)
    if n_h:
        sizes = ndi.sum(holes, lab_h, range(1, n_h + 1))
        small = np.isin(lab_h, np.where(sizes < p["max_hole_frac"] * h * w)[0] + 1)
        m |= small
    lab_c, n_c = ndi.label(m)
    comps = []
    if n_c:
        sizes = ndi.sum(m, lab_c, range(1, n_c + 1))
        big = sizes.max()
        keep = np.where(sizes >= max(p["min_component_frac"] * h * w, 0.02 * big))[0] + 1
        m = np.isin(lab_c, keep)
        comps = sorted([int(s) for s in sizes[keep - 1]], reverse=True)

    info = {
        "method": "Lab classes (lightness, warmth a*+b*, local detail) seeding GrabCut, then open/close, "
                  "small holes filled, specks dropped",
        "params": p,
        "seeds": seeds,
        "hint": used_hint or None,
        "area_frac": float(m.mean()),
        "components_px": comps,
    }
    return (m.astype(np.uint8) * 255), info


def mask_overlay(bgr, mask, colour=(0, 255, 0)):
    out = bgr.copy()
    tint = np.zeros_like(out)
    tint[:] = colour
    sel = mask > 0
    out[sel] = (0.55 * out[sel] + 0.45 * tint[sel]).astype(np.uint8)
    out[~sel] = (out[~sel] * 0.35).astype(np.uint8)
    cnts, _ = cv2.findContours((mask > 0).astype(np.uint8), cv2.RETR_LIST, cv2.CHAIN_APPROX_NONE)
    cv2.drawContours(out, cnts, -1, (0, 255, 255), 1, cv2.LINE_AA)
    return out


def render_mask(path_or_img, thresh=127):
    """A render's silhouette from starship's '--mask' shot (white on black)."""
    from common import load_bgr
    img = load_bgr(path_or_img) if isinstance(path_or_img, str) else path_or_img
    g = cv2.cvtColor(img, cv2.COLOR_BGR2GRAY)
    return ((g > thresh).astype(np.uint8) * 255)
