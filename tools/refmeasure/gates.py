"""The artist's coarse-to-fine gates, for any subject: a render against the
reference at a few blur levels, and by value masses.

- **The blur pyramid.** Both pictures are reduced to the subject's
  lightness on black (each by its own mask, so the backgrounds, which never
  match, drop out), then blurred so that only forms larger than S / N
  survive, S being the reference's spine length (or, without a sheet's
  spine, its mask's diagonal). Level N = 8 is the squint (primary forms),
  N = 32 the middle (secondary forms). Each level gives the SSIM of the
  two blurred pictures; a level that scores lower than the one above it
  says the error lives at that size.
- **The notan.** Three values: the background, and the subject split at
  its own median lightness into a dark and a light mass, each picture by
  its own median, so exposure does not count. The lightness is blurred to
  S / 32 within the subject first, so the masses are value masses, not
  texture. Compared pixel for pixel over the union of the two subjects.
- **The busy map** (the tertiary step's clustering and rest areas): Canny
  edge density within each subject, blurred to S / 16; its correlation
  between the two, and the share of each subject busier than the
  reference's median.
- **IoU** of the two masks, for reference.

`gates <sheet.json> <render> <render-mask> <out> [--levels 8,32] [--name gates]`
writes `<name>.json` and `<name>.png` (each level's blurred pair, then the
notan pair).
"""

import math
import os

import cv2
import numpy as np
from skimage.metrics import structural_similarity

from common import ensure_dir, load_bgr, put_label, read_json, save_png, write_json


def lightness(bgr, mask):
    L = cv2.cvtColor(bgr, cv2.COLOR_BGR2LAB)[..., 0].astype(np.float32) / 255.0
    return L * (mask > 0)


def notan(bgr, mask, sigma=0.0):
    """0 background, 1 the dark mass, 2 the light mass: the subject's
    lightness, blurred within the subject (normalised by the blurred mask, so
    the background does not darken the edges), split at its own median."""
    L = cv2.cvtColor(bgr, cv2.COLOR_BGR2LAB)[..., 0].astype(np.float32)
    m = mask > 0
    if sigma > 0.3:
        w = blur(m.astype(np.float32), sigma)
        L = blur(L * m, sigma) / np.maximum(w, 1e-3)
    out = np.zeros(mask.shape, np.uint8)
    if m.any():
        med = float(np.median(L[m]))
        out[m & (L <= med)] = 1
        out[m & (L > med)] = 2
    return out


def busy(bgr, mask, S):
    """Edge density within the subject, blurred to S / 16."""
    g = cv2.cvtColor(bgr, cv2.COLOR_BGR2GRAY)
    e = (cv2.Canny(g, 30, 90) > 0).astype(np.float32) * (mask > 0)
    return blur(e, S / 64.0)


def blur(img, sigma):
    return cv2.GaussianBlur(img, (0, 0), sigma) if sigma > 0.3 else img


def spine_scale(sheet, size):
    """S, the subject's size in the render's pixels."""
    sk = sheet.get("skeleton", {})
    w0 = sheet.get("image_size", [size[0], size[1]])[0]
    if sk.get("spine_length_px"):
        return sk["spine_length_px"] * size[0] / w0
    return math.hypot(*size)


def score(ref, rmask, img, mask, S, levels=(8, 32)):
    res = {"S_px": S, "iou": float(((rmask > 0) & (mask > 0)).sum() / max(1, ((rmask > 0) | (mask > 0)).sum()))}
    a, b = lightness(ref, rmask), lightness(img, mask)
    pairs = []
    for n in levels:
        sigma = S / (4.0 * n)
        ba, bb = blur(a, sigma), blur(b, sigma)
        res[f"ssim_S/{n}"] = float(structural_similarity(ba, bb, data_range=1.0, gaussian_weights=True, sigma=1.5))
        pairs.append((f"S/{n}", ba, bb))
    sigma = S / (4.0 * 32)
    sa, sb = notan(ref, rmask, sigma), notan(img, mask, sigma)
    union = (rmask > 0) | (mask > 0)
    res["notan3_agree"] = float((sa[union] == sb[union]).mean()) if union.any() else 0.0
    # The light mass's share of each subject, and its overlap
    la, lb = sa == 2, sb == 2
    res["notan_light_iou"] = float((la & lb).sum() / max(1, (la | lb).sum()))
    # The busy map (tertiary detail and rest areas): edge density blurred
    # to S / 16, its correlation between the two over their union, and each
    # one's share of busy area (density above the reference's median)
    ea, eb = busy(ref, rmask, S), busy(img, mask, S)
    if union.any():
        x, y = ea[union], eb[union]
        res["busy_corr"] = float(np.corrcoef(x, y)[0, 1]) if x.std() > 0 and y.std() > 0 else 0.0
        thr = float(np.median(ea[rmask > 0])) if (rmask > 0).any() else 0.0
        res["busy_share_ref"] = float((ea[rmask > 0] > thr).mean()) if (rmask > 0).any() else 0.0
        res["busy_share_ours"] = float((eb[mask > 0] > thr).mean()) if (mask > 0).any() else 0.0
    return res, pairs, (sa, sb)


def sheet_image(pairs, notans, scale=0.5):
    rows = []
    for name, a, b in pairs:
        row = np.hstack([a, b])
        row = cv2.cvtColor((np.clip(row * 2.0, 0, 1) * 255).astype(np.uint8), cv2.COLOR_GRAY2BGR)
        put_label(row, f"blur {name}: reference | ours", (8, 22))
        rows.append(row)
    sa, sb = notans
    pal = np.array([[0, 0, 0], [110, 70, 60], [230, 220, 210]], np.uint8)
    row = np.hstack([pal[sa], pal[sb]])
    put_label(row, "notan, 3 values: reference | ours", (8, 22))
    rows.append(row)
    out = np.vstack(rows)
    return cv2.resize(out, None, fx=scale, fy=scale, interpolation=cv2.INTER_AREA)


def cmd_gates(a):
    sheet = read_json(a.sheet)
    base = os.path.dirname(os.path.abspath(a.sheet))
    img = load_bgr(a.render)
    mask = load_bgr(a.mask)[..., 0]
    mask = ((mask > 127) * 255).astype(np.uint8)
    size = (img.shape[1], img.shape[0])
    ref = cv2.resize(load_bgr(os.path.join(base, sheet["files"]["crop"])), size, interpolation=cv2.INTER_AREA)
    rmask = load_bgr(os.path.join(base, sheet["files"]["mask"]))[..., 0]
    rmask = ((cv2.resize(rmask, size, interpolation=cv2.INTER_AREA) > 127) * 255).astype(np.uint8)
    levels = tuple(int(x) for x in a.levels.split(","))
    S = spine_scale(sheet, size)
    res, pairs, notans = score(ref, rmask, img, mask, S, levels)
    ensure_dir(a.out)
    write_json(os.path.join(a.out, a.name + ".json"), res)
    save_png(os.path.join(a.out, a.name + ".png"), sheet_image(pairs, notans))
    print(res)


def register(sub):
    s = sub.add_parser("gates"); s.add_argument("sheet"); s.add_argument("render"); s.add_argument("mask"); s.add_argument("out")
    s.add_argument("--levels", default="8,32"); s.add_argument("--name", default="gates"); s.set_defaults(fn=cmd_gates)
