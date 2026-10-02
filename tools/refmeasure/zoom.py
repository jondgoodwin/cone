"""Zooming in: the tools for finding a part's sub-structure in a crop.

Each finds one kind of pattern deterministically and draws what it found on
the crop, so it can be checked by eye:

- `period`: the repetition along a line (a spine, a wing's edge). The
  brightness is averaged across a strip about the line, detrended, and its
  autocorrelation's first strong peak gives the period; the Fourier
  transform's peak is reported beside it as a cross-check. Feeds a
  `repeatAlong` count and spacing.
- `repeats`: copies of one element. A template cut from the crop is matched
  over a few angles and scales (normalised cross-correlation), and the
  peaks are kept by non-maximum suppression. Gives each copy's position,
  angle and scale.
- `elements`: each bright, elongated element's centreline and radius. A
  ridge filter (Sato) picks out tubes and ribs, the result is thresholded
  and skeletonised, and each long skeleton piece is reported with its
  length, mean radius (the distance transform along it) and direction.
- `direction`: the structure tensor's dominant angle and coherence over the
  crop (texture.py's), the grain of the elements.
- `cropscore`: a render against the reference over the same box: the masks'
  IoU, edge density, the period along the same line in both, and the
  distance between the two gradient-orientation histograms.

Boxes and lines are in the reference picture's pixels (the sheet's cropped
picture); a render of another size is scaled to it.
"""

import math

import cv2
import numpy as np
from skimage.filters import sato
from skimage.morphology import skeletonize
from skimage.measure import label

import texture


def box_arg(s):
    return [int(round(float(v))) for v in s.split(",")]


def gray(bgr):
    return cv2.cvtColor(bgr, cv2.COLOR_BGR2LAB)[:, :, 0].astype(np.float32) / 255.0


# ---------------------------------------------------------------- period

def strip_profile(L, line, width):
    """Mean brightness across a strip `width` px wide centred on the line,
    one sample per pixel along it."""
    x0, y0, x1, y1 = line
    n = int(math.hypot(x1 - x0, y1 - y0))
    t = np.linspace(0, 1, n)
    dx, dy = (x1 - x0) / max(n, 1), (y1 - y0) / max(n, 1)
    nx, ny = -dy, dx
    nn = math.hypot(nx, ny) or 1
    nx, ny = nx / nn, ny / nn
    offs = np.linspace(-width / 2, width / 2, max(int(width), 1))
    xs = x0 + (x1 - x0) * t[:, None] + nx * offs[None, :]
    ys = y0 + (y1 - y0) * t[:, None] + ny * offs[None, :]
    v = cv2.remap(L, xs.astype(np.float32), ys.astype(np.float32), cv2.INTER_LINEAR)
    return v.mean(1)


def find_period(profile, min_p=4, max_frac=0.4):
    """Autocorrelation's first strong peak, and the Fourier peak."""
    p = profile - cv2.GaussianBlur(profile.reshape(-1, 1), (1, 0), sigmaX=1, sigmaY=12).ravel()
    p = p - p.mean()
    n = len(p)
    ac = np.correlate(p, p, "full")[n - 1:]
    ac = ac / (ac[0] + 1e-9)
    hi = int(n * max_frac)
    best, bestv = None, 0.0
    for k in range(min_p, hi):
        if ac[k] > ac[k - 1] and ac[k] >= ac[k + 1] and ac[k] > 0.1:
            best, bestv = k, float(ac[k])
            break
    spec = np.abs(np.fft.rfft(p * np.hanning(n)))
    freqs = np.fft.rfftfreq(n)
    ok = (freqs >= 1 / hi) & (freqs <= 1 / min_p)
    fpk = float(1 / freqs[ok][np.argmax(spec[ok])]) if ok.any() else None
    return {"period_px": best, "autocorr_peak": round(bestv, 3), "fft_period_px": round(fpk, 2) if fpk else None,
            "length_px": n, "count": round(n / best, 1) if best else None}, p


def draw_period(bgr, line, width, res, p):
    out = bgr.copy()
    x0, y0, x1, y1 = line
    n = res["length_px"]
    cv2.line(out, (x0, y0), (x1, y1), (0, 255, 255), 1)
    if res["period_px"]:
        # ticks at the profile's maxima spaced about one period apart
        per = res["period_px"]
        k = int(np.argmax(p[:per])) if per <= n else 0
        while k < n:
            lo, hi = max(0, k - per // 3), min(n, k + per // 3 + 1)
            k = lo + int(np.argmax(p[lo:hi]))
            t = k / max(n - 1, 1)
            x, y = x0 + (x1 - x0) * t, y0 + (y1 - y0) * t
            dx, dy = (x1 - x0) / n, (y1 - y0) / n
            cv2.line(out, (int(x - dy * width / 2), int(y + dx * width / 2)),
                     (int(x + dy * width / 2), int(y - dx * width / 2)), (255, 0, 255), 1)
            k += per
    return out


# ---------------------------------------------------------------- repeats

def find_repeats(L, tbox, angles=(-20, -10, 0, 10, 20), scales=(0.8, 1.0, 1.25), thresh=0.5, max_n=60):
    x0, y0, x1, y1 = tbox
    tmpl = L[y0:y1, x0:x1]
    th, tw = tmpl.shape
    hits = []
    for s in scales:
        for a in angles:
            M = cv2.getRotationMatrix2D((tw / 2, th / 2), a, s)
            t = cv2.warpAffine(tmpl, M, (tw, th), borderMode=cv2.BORDER_REFLECT)
            r = cv2.matchTemplate(L, t, cv2.TM_CCOEFF_NORMED)
            ys, xs = np.where(r >= thresh)
            for x, y in zip(xs, ys):
                hits.append((float(r[y, x]), int(x + tw / 2), int(y + th / 2), a, s))
    hits.sort(reverse=True)
    kept = []
    rad = 0.5 * min(tw, th)
    for h in hits:
        if all(math.hypot(h[1] - k[1], h[2] - k[2]) > rad for k in kept):
            kept.append(h)
        if len(kept) >= max_n:
            break
    return [{"score": round(h[0], 3), "x": h[1], "y": h[2], "angle": h[3], "scale": h[4]} for h in kept], (tw, th)


# ---------------------------------------------------------------- elements

def find_elements(L, sigmas=(1, 2, 3), min_len=12, pct=85):
    r = sato(L, sigmas=sigmas, black_ridges=False)
    thr = np.percentile(r, pct)
    m = r > thr
    sk = skeletonize(m)
    dt = cv2.distanceTransform(m.astype(np.uint8), cv2.DIST_L2, 3)
    lab = label(sk, connectivity=2)
    els = []
    for i in range(1, lab.max() + 1):
        ys, xs = np.where(lab == i)
        if len(xs) < min_len:
            continue
        pts = np.stack([xs, ys], 1).astype(float)
        c = pts.mean(0)
        u, s, vt = np.linalg.svd(pts - c, full_matrices=False)
        d = vt[0]
        ang = math.degrees(math.atan2(d[1], d[0]))
        ang = (ang + 90) % 180 - 90
        proj = (pts - c) @ d
        els.append({"cx": round(float(c[0]), 1), "cy": round(float(c[1]), 1), "length_px": len(xs),
                    "extent_px": round(float(proj.max() - proj.min()), 1),
                    "radius_px": round(float(dt[ys, xs].mean()), 2), "angle_deg": round(ang, 1),
                    "straightness": round(float(s[1] / (s[0] + 1e-9)), 3)})
    els.sort(key=lambda e: -e["length_px"])
    return els, sk, m


def summarise_elements(els):
    if not els:
        return {}
    a = np.array([e["angle_deg"] for e in els])
    w = np.array([e["length_px"] for e in els], float)
    # the dominant direction (axial mean, angles doubled)
    c, s = (w * np.cos(np.radians(2 * a))).sum(), (w * np.sin(np.radians(2 * a))).sum()
    dom = math.degrees(math.atan2(s, c)) / 2
    spread = 1 - math.hypot(c, s) / w.sum()
    return {"n": len(els), "median_length_px": float(np.median(w)),
            "median_radius_px": float(np.median([e["radius_px"] for e in els])),
            "dominant_angle_deg": round(dom, 1), "angle_spread": round(spread, 3)}


def draw_elements(bgr, els, sk, top=40):
    out = (bgr * 0.6).astype(np.uint8)
    out[sk] = (0, 255, 255)
    for e in els[:top]:
        a = math.radians(e["angle_deg"])
        h = e["extent_px"] / 2
        p0 = (int(e["cx"] - h * math.cos(a)), int(e["cy"] - h * math.sin(a)))
        p1 = (int(e["cx"] + h * math.cos(a)), int(e["cy"] + h * math.sin(a)))
        cv2.line(out, p0, p1, (255, 0, 255), 1)
        cv2.circle(out, (int(e["cx"]), int(e["cy"])), max(1, int(e["radius_px"])), (0, 200, 0), 1)
    return out


# ---------------------------------------------------------------- direction

def direction(L, sigma=3.0):
    jxx, jyy, jxy = texture.structure_tensor(L, sigma)
    a, b, c = jxx.mean(), jyy.mean(), jxy.mean()
    # the grain runs across the dominant gradient
    ang = 0.5 * math.degrees(math.atan2(2 * c, a - b)) + 90
    ang = (ang + 90) % 180 - 90
    coh = math.sqrt((a - b) ** 2 + 4 * c * c) / (a + b + 1e-9)
    return {"grain_deg": round(ang, 1), "coherence": round(float(coh), 3)}


# ---------------------------------------------------------------- crop score

def orient_hist(L, mask, bins=18):
    gx = cv2.Sobel(L, cv2.CV_32F, 1, 0, ksize=3)
    gy = cv2.Sobel(L, cv2.CV_32F, 0, 1, ksize=3)
    mag = np.hypot(gx, gy)
    ang = (np.degrees(np.arctan2(gy, gx)) % 180)
    h, _ = np.histogram(ang[mask], bins=bins, range=(0, 180), weights=mag[mask])
    return h / (h.sum() + 1e-9)


def edge_density(L, mask):
    e = cv2.Canny((L * 255).astype(np.uint8), 40, 100) > 0
    return float(e[mask].mean()) if mask.any() else 0.0


# ---------------------------------------------------------------- rectifying

def resample_polyline(img_pts, arc):
    """Image points re-spaced so that equal steps are equal true (3-D) arc
    length: `arc` is the cumulative 3-D length at each image point. One
    sample per image pixel on average."""
    img_pts = np.asarray(img_pts, float)
    seg = np.hypot(*np.diff(img_pts, axis=0).T).sum()
    n = max(int(seg), 2)
    s = np.linspace(arc[0], arc[-1], n)
    return np.stack([np.interp(s, arc, img_pts[:, 0]), np.interp(s, arc, img_pts[:, 1])], 1), (arc[-1] - arc[0]) / (n - 1)


def polyline_profile(L, pts, width):
    """Mean brightness across a strip about a polyline, one sample per point."""
    pts = np.asarray(pts, float)
    d = np.gradient(pts, axis=0)
    d /= np.linalg.norm(d, axis=1, keepdims=True) + 1e-9
    nrm = np.stack([-d[:, 1], d[:, 0]], 1)
    offs = np.linspace(-width / 2, width / 2, max(int(width), 1))
    xs = pts[:, 0:1] + nrm[:, 0:1] * offs[None, :]
    ys = pts[:, 1:2] + nrm[:, 1:2] * offs[None, :]
    v = cv2.remap(L, xs.astype(np.float32), ys.astype(np.float32), cv2.INTER_LINEAR)
    return v.mean(1)


def plane_homography(cam_proj, p0, p1, p2, out_w, units_per_px=None):
    """A homography from the picture to the plane through three 3-D points:
    plane coordinates u along p0->p1 and v across, in the plane. Returns the
    3x3 matrix (picture -> plane pixels), the output size and units per
    plane pixel."""
    p0, p1, p2 = (np.asarray(p, float) for p in (p0, p1, p2))
    u = p1 - p0
    lu = np.linalg.norm(u)
    u /= lu
    w = p2 - p0
    vv = w - (w @ u) * u
    vv /= np.linalg.norm(vv)
    corners3 = [p0, p1, p2]
    uv = np.array([[(c - p0) @ u, (c - p0) @ vv] for c in corners3])
    # a box in the plane holding the three points, with a margin
    lo, hi = uv.min(0) - 0.15 * lu, uv.max(0) + 0.15 * lu
    k = units_per_px or (hi[0] - lo[0]) / out_w
    W, H = int((hi[0] - lo[0]) / k), int((hi[1] - lo[1]) / k)
    quad3 = [p0 + u * a + vv * b for a, b in ((lo[0], lo[1]), (hi[0], lo[1]), (hi[0], hi[1]), (lo[0], hi[1]))]
    src = cam_proj(np.array(quad3)).astype(np.float32)
    dst = np.float32([[0, 0], [W, 0], [W, H], [0, H]])
    return cv2.getPerspectiveTransform(src, dst), (W, H), k


# ---------------------------------------------------------------- cross-sections

def cross_section(L, centre, axis_deg, half, n=41):
    """Brightness across a part, perpendicular to its axis, and what its
    shape says: a centred peak with a smooth falloff reads round; a
    plateau, flat; a sharp step, a ridge or an edge."""
    a = math.radians(axis_deg + 90)
    t = np.linspace(-half, half, n)
    xs = (centre[0] + t * math.cos(a)).astype(np.float32).reshape(1, -1)
    ys = (centre[1] + t * math.sin(a)).astype(np.float32).reshape(1, -1)
    v = cv2.remap(cv2.GaussianBlur(L, (0, 0), 1.0), xs, ys, cv2.INTER_LINEAR).ravel()
    lo, hi = float(v.min()), float(v.max())
    nv = (v - lo) / (hi - lo + 1e-9)
    plateau = float((nv > 0.85).mean())
    step = float(np.abs(np.diff(nv)).max())
    peak = float(t[int(np.argmax(nv))] / half)
    # how well a cosine bump (a lit cylinder) explains it, against a step
    bump = np.cos(np.clip((t - t[int(np.argmax(nv))]) / half * math.pi / 2, -math.pi / 2, math.pi / 2))
    r_bump = float(np.corrcoef(nv, bump)[0, 1])
    if step > 0.45:
        kind = "step (ridge or edge)"
    elif plateau > 0.35:
        kind = "flat (plateau)"
    elif r_bump > 0.6:
        kind = "round (smooth peak)"
    else:
        kind = "mixed"
    return {"centre": [round(float(c), 1) for c in centre], "axis_deg": axis_deg, "half_px": half,
            "contrast": round(hi - lo, 3), "plateau_frac": round(plateau, 3), "max_step": round(step, 3),
            "peak_at": round(peak, 2), "fit_round": round(r_bump, 3), "reads": kind,
            "profile": [round(float(x), 3) for x in nv]}


# ---------------------------------------------------------------- commands

def _out(path):
    import os
    os.makedirs(path, exist_ok=True)
    return path


def _projector(arg, size):
    import camera as cam
    e, t, u, f = cam.parse_arg(arg)
    return lambda pts: cam.project(pts, e, t, u, f, size[0], size[1])


def cmd_crop(a):
    from common import load_bgr, save_png
    img = load_bgr(a.image)
    x0, y0, x1, y1 = box_arg(a.box)
    c = img[y0:y1, x0:x1]
    if a.scale != 1:
        c = cv2.resize(c, None, fx=a.scale, fy=a.scale, interpolation=cv2.INTER_CUBIC)
    save_png(a.out, c)


def cmd_period(a):
    import os
    from common import load_bgr, save_png, write_json
    import camera as cam
    img = load_bgr(a.image)
    L = gray(img)
    out = _out(a.out)
    res = {"image": os.path.abspath(a.image), "width_px": a.width}
    if a.spine:
        proj = _projector(a.camera, (img.shape[1], img.shape[0]))
        pts3, arc = cam.starship_spine(a.t0, a.t1, 400, a.lift)
        ip = proj(pts3)
        if a.raw:   # equal image steps, to compare with the rectified profile
            arc = np.concatenate([[0.0], np.cumsum(np.hypot(*np.diff(ip, axis=0).T))])
        pts, step = resample_polyline(ip, arc)
        prof = polyline_profile(L, pts, a.width)
        r, p = find_period(prof, min_p=a.min_period)
        res.update(r)
        res["sampling"] = "equal image px" if a.raw else "equal true arc length (rectified)"
        res["units_per_sample"] = round(float(step), 5)
        if r["period_px"]:
            res["period_units"] = round(r["period_px"] * step, 4)
        # a drifting period shows as different periods in the two halves
        h = len(prof) // 2
        res["period_first_half"] = find_period(prof[:h], min_p=a.min_period)[0]["period_px"]
        res["period_second_half"] = find_period(prof[h:], min_p=a.min_period)[0]["period_px"]
        o = img.copy()
        cv2.polylines(o, [pts.astype(np.int32)], False, (0, 255, 255), 1)
        if r["period_px"]:
            per = r["period_px"]
            k = int(np.argmax(p[:per]))
            while k < len(pts):
                lo, hi = max(0, k - per // 3), min(len(pts), k + per // 3 + 1)
                k = lo + int(np.argmax(p[lo:hi]))
                cv2.circle(o, tuple(int(v) for v in pts[k]), 3, (255, 0, 255), 1)
                k += per
    else:
        line = box_arg(a.line)
        prof = strip_profile(L, line, a.width)
        r, p = find_period(prof, min_p=a.min_period)
        res.update(r)
        res["line"] = line
        o = draw_period(img, line, a.width, r, p)
    if a.box:
        x0, y0, x1, y1 = box_arg(a.box)
        o = o[y0:y1, x0:x1]
    save_png(os.path.join(out, a.name + ".png"), o)
    write_json(os.path.join(out, a.name + ".json"), res)
    print({k: v for k, v in res.items() if k != "image"})


def cmd_repeats(a):
    import os
    from common import load_bgr, save_png, write_json
    img = load_bgr(a.image)
    x0, y0, x1, y1 = box_arg(a.box)
    c = img[y0:y1, x0:x1]
    L = gray(c)
    tb = box_arg(a.template)
    tb = [tb[0] - x0, tb[1] - y0, tb[2] - x0, tb[3] - y0]
    hits, _ = find_repeats(L, tb, thresh=a.thresh)
    o = c.copy()
    cv2.rectangle(o, (tb[0], tb[1]), (tb[2], tb[3]), (0, 255, 255), 1)
    for h in hits:
        cv2.circle(o, (h["x"], h["y"]), 3, (255, 0, 255), -1)
    xs = np.array([[h["x"], h["y"]] for h in hits], float)
    res = {"box": [x0, y0, x1, y1], "template": tb, "n": len(hits), "hits": hits}
    if len(xs) > 2:
        # nearest-neighbour spacing: the copies' pitch
        d = np.sqrt(((xs[:, None] - xs[None]) ** 2).sum(-1))
        np.fill_diagonal(d, 1e9)
        res["nn_spacing_px_median"] = round(float(np.median(d.min(1))), 1)
    out = _out(a.out)
    save_png(os.path.join(out, a.name + ".png"), o)
    write_json(os.path.join(out, a.name + ".json"), res)
    print({k: v for k, v in res.items() if k != "hits"})


def cmd_elements(a):
    import os
    from common import load_bgr, save_png, write_json
    img = load_bgr(a.image)
    x0, y0, x1, y1 = box_arg(a.box)
    c = img[y0:y1, x0:x1]
    L = gray(c)
    els, sk, _ = find_elements(L, min_len=a.min_len, pct=a.pct)
    res = {"box": [x0, y0, x1, y1], "summary": summarise_elements(els), "direction": direction(L),
           "elements": els[:60]}
    out = _out(a.out)
    save_png(os.path.join(out, a.name + ".png"), draw_elements(c, els, sk))
    write_json(os.path.join(out, a.name + ".json"), res)
    print(res["summary"], res["direction"])


def cmd_rectify(a):
    import os
    from common import load_bgr, save_png, write_json
    img = load_bgr(a.image)
    proj = _projector(a.camera, (img.shape[1], img.shape[0]))
    pts = [np.array([float(v) for v in s.split(",")]) for s in a.plane]
    H, (W, Hh), k = plane_homography(proj, *pts, out_w=a.width)
    warp = cv2.warpPerspective(img, H, (W, Hh))
    out = _out(a.out)
    save_png(os.path.join(out, a.name + ".png"), warp)
    write_json(os.path.join(out, a.name + ".json"), {"plane": [list(map(float, p)) for p in pts], "size": [W, Hh],
                                                       "units_per_px": k, "homography": H.tolist()})
    print(f"rectified {W}x{Hh}, {k:.4f} units per px")


def cmd_profile(a):
    import os
    from common import load_bgr, save_png, write_json
    img = load_bgr(a.image)
    L = gray(img)
    res = []
    o = img.copy()
    for s in a.at:
        x, y, ang, half = (float(v) for v in s.split(","))
        r = cross_section(L, (x, y), ang, half)
        res.append(r)
        aa = math.radians(ang + 90)
        p0 = (int(x - half * math.cos(aa)), int(y - half * math.sin(aa)))
        p1 = (int(x + half * math.cos(aa)), int(y + half * math.sin(aa)))
        cv2.line(o, p0, p1, (255, 0, 255), 1)
        cv2.putText(o, r["reads"].split()[0], (p1[0] + 2, p1[1]), cv2.FONT_HERSHEY_SIMPLEX, 0.35, (0, 255, 255), 1)
    out = _out(a.out)
    if a.box:
        x0, y0, x1, y1 = box_arg(a.box)
        o = o[y0:y1, x0:x1]
    save_png(os.path.join(out, a.name + ".png"), o)
    write_json(os.path.join(out, a.name + ".json"), res)
    for r in res:
        print(r["centre"], r["reads"], "plateau", r["plateau_frac"], "step", r["max_step"], "round-fit", r["fit_round"])


def cmd_cropscore(a):
    """The reference and a render over one box (reference pixels): IoU,
    edge density, direction, element summary, and the orientation
    histograms' L1 distance; both crops side by side."""
    import os
    from common import load_bgr, save_png, write_json
    import refmeasure as rm
    rbgr = load_bgr(a.render)
    rmask = load_bgr(a.mask)[:, :, 0] > 127
    size = (rbgr.shape[1], rbgr.shape[0])
    sheet, ref, refmask, _ = rm.load_reference(a.sheet, size)
    refmask = refmask > 127
    sx = size[0] / sheet["image_size"][0]
    x0, y0, x1, y1 = [int(round(v * sx)) for v in box_arg(a.box)]
    out = {"box_ref_px": box_arg(a.box), "render_size": list(size)}
    pair, hists = [], {}
    for tag, im, m in (("ref", ref, refmask), ("ours", rbgr, rmask)):
        c, cm = im[y0:y1, x0:x1], m[y0:y1, x0:x1]
        L = gray(c)
        els, sk, _ = find_elements(L, min_len=a.min_len)
        out[tag] = {"edge_density": round(edge_density(L, cm), 4), "direction": direction(L),
                    "elements": summarise_elements([e for e in els if cm[int(e["cy"]), int(e["cx"])]])}
        hists[tag] = orient_hist(L, cm)
        pair.append(draw_elements(c, els, sk))
    rm_, om = refmask[y0:y1, x0:x1], rmask[y0:y1, x0:x1]
    out["iou"] = round(float((rm_ & om).sum() / max((rm_ | om).sum(), 1)), 4)
    out["orient_hist_l1"] = round(float(np.abs(hists["ref"] - hists["ours"]).sum()), 4)
    if a.line:
        lx = [int(round(v * sx)) for v in box_arg(a.line)]
        for tag, im in (("ref", ref), ("ours", rbgr)):
            out[tag]["period"] = find_period(strip_profile(gray(im), lx, a.width * sx), min_p=a.min_period)[0]
    od = _out(a.out)
    gap = np.full((pair[0].shape[0], 6, 3), 255, np.uint8)
    save_png(os.path.join(od, a.name + ".png"), np.hstack([pair[0], gap, pair[1]]))
    save_png(os.path.join(od, a.name + "-plain.png"), np.hstack([ref[y0:y1, x0:x1], gap, rbgr[y0:y1, x0:x1]]))
    write_json(os.path.join(od, a.name + ".json"), out)
    import json
    print(json.dumps(out, separators=(",", ":"))[:900])


def register(sub):
    s = sub.add_parser("crop"); s.add_argument("image"); s.add_argument("box"); s.add_argument("out")
    s.add_argument("--scale", type=float, default=1.0); s.set_defaults(fn=cmd_crop)
    s = sub.add_parser("period"); s.add_argument("image"); s.add_argument("out")
    s.add_argument("--line"); s.add_argument("--spine", action="store_true"); s.add_argument("--camera")
    s.add_argument("--t0", type=float, default=0.0); s.add_argument("--t1", type=float, default=1.0)
    s.add_argument("--lift", type=float, default=0.0); s.add_argument("--raw", action="store_true")
    s.add_argument("--width", type=float, default=9); s.add_argument("--min-period", type=int, default=4)
    s.add_argument("--box"); s.add_argument("--name", default="period"); s.set_defaults(fn=cmd_period)
    s = sub.add_parser("repeats"); s.add_argument("image"); s.add_argument("out"); s.add_argument("--box", required=True)
    s.add_argument("--template", required=True); s.add_argument("--thresh", type=float, default=0.5)
    s.add_argument("--name", default="repeats"); s.set_defaults(fn=cmd_repeats)
    s = sub.add_parser("elements"); s.add_argument("image"); s.add_argument("out"); s.add_argument("--box", required=True)
    s.add_argument("--min-len", type=int, default=12); s.add_argument("--pct", type=float, default=85)
    s.add_argument("--name", default="elements"); s.set_defaults(fn=cmd_elements)
    s = sub.add_parser("rectify"); s.add_argument("image"); s.add_argument("out"); s.add_argument("--camera", required=True)
    s.add_argument("--plane", nargs=3, required=True, help="three 3-D points x,y,z")
    s.add_argument("--width", type=int, default=600); s.add_argument("--name", default="rectified"); s.set_defaults(fn=cmd_rectify)
    s = sub.add_parser("profile"); s.add_argument("image"); s.add_argument("out")
    s.add_argument("--at", action="append", required=True, help="x,y,axis_deg,half_px (repeatable)")
    s.add_argument("--box"); s.add_argument("--name", default="profile"); s.set_defaults(fn=cmd_profile)
    s = sub.add_parser("cropscore"); s.add_argument("sheet"); s.add_argument("render"); s.add_argument("mask"); s.add_argument("out")
    s.add_argument("--box", required=True); s.add_argument("--line"); s.add_argument("--width", type=float, default=9)
    s.add_argument("--min-period", type=int, default=4); s.add_argument("--min-len", type=int, default=12)
    s.add_argument("--name", default="crop"); s.set_defaults(fn=cmd_cropscore)
