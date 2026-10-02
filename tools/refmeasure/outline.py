"""The outline: contour tracing, Ramer-Douglas-Peucker, then cubic Beziers.

The outer contour of each mask component is traced (OpenCV, every pixel),
lightly smoothed, and simplified by RDP; the RDP vertices where the outline
turns sharply are kept as corners. Each run between corners is fitted with
cubic Beziers by Schneider's algorithm (Graphics Gems, 1990): chord-length
parameters, a least-squares fit with the end tangents fixed, Newton-Raphson
reparameterisation, and a split at the worst point while the error exceeds
the bound. Each segment records its largest distance to the traced points.
"""

import math

import cv2
import numpy as np


def _bezier(ctrl, t):
    t = t[:, None]
    mt = 1 - t
    return (mt ** 3) * ctrl[0] + 3 * (mt ** 2) * t * ctrl[1] + 3 * mt * (t ** 2) * ctrl[2] + (t ** 3) * ctrl[3]


def _bezier_d1(ctrl, t):
    t = t[:, None]
    mt = 1 - t
    return 3 * (mt ** 2) * (ctrl[1] - ctrl[0]) + 6 * mt * t * (ctrl[2] - ctrl[1]) + 3 * (t ** 2) * (ctrl[3] - ctrl[2])


def _bezier_d2(ctrl, t):
    t = t[:, None]
    return 6 * (1 - t) * (ctrl[2] - 2 * ctrl[1] + ctrl[0]) + 6 * t * (ctrl[3] - 2 * ctrl[2] + ctrl[1])


def _chord_params(pts):
    d = np.r_[0, np.cumsum(np.linalg.norm(np.diff(pts, axis=0), axis=1))]
    return d / d[-1] if d[-1] > 0 else np.linspace(0, 1, len(pts))


def _fit_one(pts, u, t1, t2):
    p0, p3 = pts[0], pts[-1]
    a1 = np.outer(3 * (1 - u) ** 2 * u, t1)
    a2 = np.outer(3 * (1 - u) * u ** 2, t2)
    c00 = (a1 * a1).sum(); c01 = (a1 * a2).sum(); c11 = (a2 * a2).sum()
    base = _bezier(np.array([p0, p0, p3, p3]), u)
    tmp = pts - base
    x0 = (a1 * tmp).sum(); x1 = (a2 * tmp).sum()
    det = c00 * c11 - c01 * c01
    seg = np.linalg.norm(p3 - p0)
    if abs(det) > 1e-12:
        al1 = (x0 * c11 - x1 * c01) / det
        al2 = (c00 * x1 - c01 * x0) / det
    else:
        al1 = al2 = seg / 3
    if al1 < 1e-6 * seg or al2 < 1e-6 * seg:
        al1 = al2 = seg / 3
    return np.array([p0, p0 + al1 * t1, p3 + al2 * t2, p3])


def _max_error(ctrl, pts, u):
    d = np.linalg.norm(_bezier(ctrl, u) - pts, axis=1)
    i = int(np.argmax(d))
    return float(d[i]), i


def _reparam(ctrl, pts, u):
    q = _bezier(ctrl, u) - pts
    d1 = _bezier_d1(ctrl, u)
    d2 = _bezier_d2(ctrl, u)
    num = (q * d1).sum(1)
    den = (d1 * d1).sum(1) + (q * d2).sum(1)
    with np.errstate(divide="ignore", invalid="ignore"):
        nu = np.where(np.abs(den) > 1e-12, u - num / den, u)
    return np.clip(nu, 0, 1)


def _unit(v):
    n = np.linalg.norm(v)
    return v / n if n > 1e-12 else v


def fit_cubic(pts, t1, t2, err, out, depth=0):
    """Schneider: fit pts (N x 2) with end tangents t1 (out of the start)
    and t2 (into the end, pointing backwards), appending to out."""
    if len(pts) == 2:
        dist = np.linalg.norm(pts[1] - pts[0]) / 3
        ctrl = np.array([pts[0], pts[0] + t1 * dist, pts[1] + t2 * dist, pts[1]])
        out.append((ctrl, 0.0))
        return
    u = _chord_params(pts)
    ctrl = _fit_one(pts, u, t1, t2)
    e, split = _max_error(ctrl, pts, u)
    if e < err:
        out.append((ctrl, e))
        return
    if e < 4 * err:
        for _ in range(8):
            u = _reparam(ctrl, pts, u)
            ctrl = _fit_one(pts, u, t1, t2)
            e, split = _max_error(ctrl, pts, u)
            if e < err:
                out.append((ctrl, e))
                return
    if depth > 40 or len(pts) < 4:
        out.append((ctrl, e))
        return
    split = min(max(split, 1), len(pts) - 2)
    tc = _unit(pts[split - 1] - pts[split + 1])
    fit_cubic(pts[:split + 1], t1, tc, err, out, depth + 1)
    fit_cubic(pts[split:], -tc, t2, err, out, depth + 1)


def outline(mask, err_px=2.0, rdp_eps=2.0, corner_deg=55.0, smooth=2, min_area_frac=0.002):
    """Every outer contour of the mask as RDP polylines and Bezier segments."""
    m = (mask > 0).astype(np.uint8)
    cnts, _ = cv2.findContours(m, cv2.RETR_EXTERNAL, cv2.CHAIN_APPROX_NONE)
    area_min = min_area_frac * m.size
    results = []
    for c in sorted(cnts, key=cv2.contourArea, reverse=True):
        if cv2.contourArea(c) < area_min:
            continue
        pts = c[:, 0, :].astype(np.float64)
        # light circular smoothing against pixel staircase
        if smooth > 0 and len(pts) > 4 * smooth:
            k = 2 * smooth + 1
            ext = np.vstack([pts[-smooth:], pts, pts[:smooth]])
            ker = np.ones(k) / k
            pts = np.stack([np.convolve(ext[:, 0], ker, "valid"), np.convolve(ext[:, 1], ker, "valid")], 1)
        rdp = cv2.approxPolyDP(pts.astype(np.float32).reshape(-1, 1, 2), rdp_eps, True)[:, 0, :]
        # Corners: RDP vertices where the polyline turns by more than corner_deg
        n = len(rdp)
        corner_idx = []
        tree_idx = [int(np.argmin(np.linalg.norm(pts - v, axis=1))) for v in rdp]
        for i in range(n):
            a, b, cc = rdp[i - 1], rdp[i], rdp[(i + 1) % n]
            v1, v2 = b - a, cc - b
            ang = math.degrees(math.atan2(v1[0] * v2[1] - v1[1] * v2[0], v1[0] * v2[0] + v1[1] * v2[1]))
            if abs(ang) > corner_deg:
                corner_idx.append(tree_idx[i])
        corner_idx = sorted(set(corner_idx))
        if len(corner_idx) < 2:
            corner_idx = sorted(set([0, len(pts) // 2] + corner_idx))
        segs = []
        for j in range(len(corner_idx)):
            i0 = corner_idx[j]
            i1 = corner_idx[(j + 1) % len(corner_idx)]
            run = pts[i0:i1 + 1] if i1 > i0 else np.vstack([pts[i0:], pts[:i1 + 1]])
            if len(run) < 2:
                continue
            look = min(4, len(run) - 1)
            t1 = _unit(run[look] - run[0])
            t2 = _unit(run[-1 - look] - run[-1])
            fit_cubic(run, t1, t2, err_px, segs)
        results.append({
            "area_px": float(cv2.contourArea(c)),
            "traced_points": int(len(pts)),
            "rdp": rdp,
            "corners": [pts[i] for i in corner_idx],
            "beziers": [{"p": ctrl, "max_err_px": e} for ctrl, e in segs],
        })
    return results


def draw_outline(bgr, outlines, scale=1.0):
    out = (bgr.astype(np.float32) * 0.45).astype(np.uint8)
    for o in outlines:
        for s in o["beziers"]:
            ctrl = np.asarray(s["p"]) * scale
            curve = _bezier(ctrl, np.linspace(0, 1, 24))
            col = (0, 255, 255) if s["max_err_px"] <= 2.0 else (0, 128, 255)
            cv2.polylines(out, [np.round(curve).astype(np.int32)], False, col, 2, cv2.LINE_AA)
            cv2.line(out, tuple(np.round(ctrl[0]).astype(int)), tuple(np.round(ctrl[1]).astype(int)), (255, 120, 0), 1, cv2.LINE_AA)
            cv2.line(out, tuple(np.round(ctrl[3]).astype(int)), tuple(np.round(ctrl[2]).astype(int)), (255, 120, 0), 1, cv2.LINE_AA)
            cv2.circle(out, tuple(np.round(ctrl[0]).astype(int)), 3, (0, 0, 255), -1)
        for cpt in o["corners"]:
            cv2.drawMarker(out, tuple(np.round(np.asarray(cpt) * scale).astype(int)), (255, 255, 255), cv2.MARKER_SQUARE, 9, 1)
    return out
