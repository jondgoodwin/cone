"""The skeleton with radii: the medial axis of the mask, pruned, classified.

The medial axis (skimage) gives at each skeleton pixel the radius of the
largest disc inside the mask there: the body's half-thickness. Terminal
branches shorter than a multiple of the radius where they join are spurs of
the outline's roughness and are pruned, repeatedly. The spine is the
shortest skeleton path between the head end and the tail end (given by hint
points, or chosen automatically: the pair of endpoints whose path is long and
runs along the mask's major axis, the blunter end being the head). What
branches off the spine is grouped into side trees; the two largest attached
mid-body are the wings, the larger-area one "near". Angles are measured in the
image plane.
"""

import math

import cv2
import numpy as np
from scipy import ndimage as ndi
from scipy.sparse import coo_matrix
from scipy.sparse.csgraph import dijkstra
from skimage.morphology import medial_axis, skeletonize

N8 = [(-1, -1), (-1, 0), (-1, 1), (0, -1), (0, 1), (1, -1), (1, 0), (1, 1)]

# Where along the spine (0 head tip, 1 tail tip) each body region ends
SPINE_REGIONS = [("head", 0.10), ("neck", 0.25), ("body", 0.62), ("tail", 1.01)]


def _neighbours(sk):
    k = np.ones((3, 3), np.uint8)
    k[1, 1] = 0
    return cv2.filter2D(sk.astype(np.uint8), -1, k, borderType=cv2.BORDER_CONSTANT)


def _prune(sk, dist, k_radius=1.6, min_px=12, rounds=12):
    sk = sk.copy()
    for _ in range(rounds):
        nb = _neighbours(sk)
        ends = np.argwhere(sk & (nb == 1))
        removed = False
        for y, x in ends:
            path = [(y, x)]
            py, px = -1, -1
            cy, cx = y, x
            junction_r = None
            while True:
                nxt = [(cy + dy, cx + dx) for dy, dx in N8
                       if 0 <= cy + dy < sk.shape[0] and 0 <= cx + dx < sk.shape[1]
                       and sk[cy + dy, cx + dx] and (cy + dy, cx + dx) != (py, px) and (cy + dy, cx + dx) not in path[-3:]]
                if len(nxt) != 1:
                    if len(nxt) >= 2:
                        junction_r = dist[cy, cx]
                    break
                ny, nx_ = nxt[0]
                if nb[ny, nx_] >= 3:
                    junction_r = dist[ny, nx_]
                    break
                py, px = cy, cx
                cy, cx = ny, nx_
                path.append((cy, cx))
                if len(path) > 4000:
                    break
            if junction_r is None:
                continue  # an isolated piece; leave it
            if len(path) < max(min_px, k_radius * junction_r):
                for (yy, xx) in path:
                    sk[yy, xx] = False
                removed = True
        if not removed:
            break
        sk = skeletonize(sk)
    return sk


def _pixel_graph(sk):
    idx = -np.ones(sk.shape, np.int64)
    pts = np.argwhere(sk)
    idx[pts[:, 0], pts[:, 1]] = np.arange(len(pts))
    rows, cols, w = [], [], []
    for dy, dx in N8:
        ys, xs = pts[:, 0] + dy, pts[:, 1] + dx
        ok = (ys >= 0) & (ys < sk.shape[0]) & (xs >= 0) & (xs < sk.shape[1])
        j = np.full(len(pts), -1)
        j[ok] = idx[ys[ok], xs[ok]]
        sel = j >= 0
        rows.append(np.where(sel)[0]); cols.append(j[sel]); w.append(np.full(sel.sum(), math.hypot(dy, dx)))
    g = coo_matrix((np.concatenate(w), (np.concatenate(rows), np.concatenate(cols))), shape=(len(pts), len(pts))).tocsr()
    return pts, idx, g


def _path(pred, i, j):
    out = [j]
    while out[-1] != i and out[-1] >= 0:
        out.append(pred[out[-1]])
    return out[::-1] if out[-1] == i else []


def principal_axes(mask):
    m = cv2.moments((mask > 0).astype(np.uint8), True)
    cx, cy = m["m10"] / m["m00"], m["m01"] / m["m00"]
    mu20, mu02, mu11 = m["mu20"] / m["m00"], m["mu02"] / m["m00"], m["mu11"] / m["m00"]
    cov = np.array([[mu20, mu11], [mu11, mu02]])
    ev, evec = np.linalg.eigh(cov)
    major = evec[:, 1]
    ang = math.degrees(math.atan2(major[1], major[0]))
    return {
        "centroid_px": [cx, cy],
        "major_axis_deg_image": ang,   # image angle, x right, y down, degrees
        "major_len_px": 4 * math.sqrt(ev[1]),
        "minor_len_px": 4 * math.sqrt(max(ev[0], 0)),
        "elongation": math.sqrt(ev[1] / max(ev[0], 1e-9)),
        "area_px": m["m00"],
    }


def _angle_between(u, v):
    a = math.degrees(math.atan2(u[0] * v[1] - u[1] * v[0], u[0] * v[0] + u[1] * v[1]))
    return a


def analyse(mask, hint=None, smooth_px=3, stations=21):
    m = (mask > 0)
    if smooth_px:
        m = cv2.GaussianBlur(m.astype(np.float32), (0, 0), smooth_px) > 0.5
    sk, dist = medial_axis(m, return_distance=True)
    sk = _prune(sk, dist)
    # keep the largest skeleton piece
    lab, n = ndi.label(sk, structure=np.ones((3, 3)))
    if n > 1:
        sizes = ndi.sum(sk, lab, range(1, n + 1))
        sk = lab == (np.argmax(sizes) + 1)
    pts, idx, g = _pixel_graph(sk)
    nb = _neighbours(sk)
    ends = [int(idx[y, x]) for y, x in np.argwhere(sk & (nb == 1))]
    axes = principal_axes(m)
    major = np.array([math.cos(math.radians(axes["major_axis_deg_image"])), math.sin(math.radians(axes["major_axis_deg_image"]))])

    how = "hint"
    if hint and "head" in hint and "tail" in hint:
        def nearest(p):
            d = np.linalg.norm(pts[:, ::-1] - np.asarray(p, float), axis=1)
            return int(np.argmin(d))
        hi, ti = nearest(hint["head"]), nearest(hint["tail"])
        dmat, pred = dijkstra(g, indices=[hi], return_predecessors=True)
        spine = _path(pred[0], hi, ti)
    else:
        how = "automatic"
        dmat, pred = dijkstra(g, indices=ends, return_predecessors=True)
        best, pair = -1, None
        for a_i, a in enumerate(ends):
            for b in ends:
                if b <= a or not np.isfinite(dmat[a_i, b]):
                    continue
                chord = (pts[b] - pts[a])[::-1].astype(float)
                c = abs(chord @ major) / (np.linalg.norm(chord) + 1e-9)
                score = dmat[a_i, b] * c ** 2
                if score > best:
                    best, pair = score, (a_i, a, b)
        a_i, a, b = pair
        spine = _path(pred[a_i], a, b)
        # the blunter end (larger mean radius over its first tenth) is the head
        k = max(3, len(spine) // 10)
        ra = np.mean([dist[tuple(pts[i])] for i in spine[:k]])
        rb = np.mean([dist[tuple(pts[i])] for i in spine[-k:]])
        if rb > ra:
            spine = spine[::-1]

    sp = pts[spine]                      # (row, col)
    sxy = sp[:, ::-1].astype(float)      # (x, y)
    seglen = np.r_[0, np.cumsum(np.linalg.norm(np.diff(sxy, axis=0), axis=1))]
    L = seglen[-1]
    tpar = seglen / L
    rad = dist[sp[:, 0], sp[:, 1]]

    # The radius at stations along the spine, a median over +-2% of it
    prof = []
    for t in np.linspace(0, 1, stations):
        sel = np.abs(tpar - t) <= 0.02
        r = float(np.median(rad[sel])) if sel.any() else float("nan")
        j = int(np.argmin(np.abs(tpar - t)))
        prof.append({"t": t, "radius_px": r, "radius_of_spine": r / L, "at_px": sxy[j]})

    def tangent_at(t, span=0.05):
        j0 = int(np.argmin(np.abs(tpar - max(0, t - span))))
        j1 = int(np.argmin(np.abs(tpar - min(1, t + span))))
        v = sxy[j1] - sxy[j0]
        return v / (np.linalg.norm(v) + 1e-9)

    # Side trees: the skeleton less the spine
    spine_mask = np.zeros_like(sk)
    spine_mask[sp[:, 0], sp[:, 1]] = True
    near_spine = cv2.dilate(spine_mask.astype(np.uint8), np.ones((3, 3), np.uint8)) > 0
    side = sk & ~spine_mask
    slab, ns = ndi.label(side, structure=np.ones((3, 3)))
    branches = []
    for k in range(1, ns + 1):
        comp = slab == k
        touch = np.argwhere(comp & near_spine)
        if len(touch) == 0:
            continue
        cpts = np.argwhere(comp)
        sub = -np.ones(sk.shape, np.int64)
        sub[cpts[:, 0], cpts[:, 1]] = np.arange(len(cpts))
        _, _, gc = _pixel_graph(comp)
        root = int(sub[touch[0][0], touch[0][1]])
        dd, pr = dijkstra(gc, indices=[root], return_predecessors=True)
        dd = dd[0]
        tip = int(np.argmax(np.where(np.isfinite(dd), dd, -1)))
        length = float(dd[tip])
        rootxy = cpts[root][::-1].astype(float)
        tipxy = cpts[tip][::-1].astype(float)
        # where on the spine it attaches
        j = int(np.argmin(np.linalg.norm(sxy - rootxy, axis=1)))
        t_att = float(tpar[j])
        tan = tangent_at(t_att)
        chord = tipxy - rootxy
        ang = _angle_between(tan, chord / (np.linalg.norm(chord) + 1e-9))
        path = _path(pr[0], root, tip)
        brad = dist[cpts[path][:, 0], cpts[path][:, 1]] if path else np.array([0.0])
        branches.append({
            "id": k,
            "attach_t": t_att,
            "root_px": rootxy, "tip_px": tipxy,
            "length_px": length, "length_of_spine": length / L,
            # 0: pointing straight towards the tail along the spine; 180: towards the head
            "angle_to_spine_deg": ang,
            "side": "left" if ang < 0 else "right",
            "mean_radius_px": float(np.mean(brad)),
            "path_px": cpts[path][:, ::-1] if path else np.zeros((0, 2)),
            "_pixels": comp,
        })
    branches.sort(key=lambda b: -b["length_px"])
    major_br = [b for b in branches if b["length_of_spine"] >= 0.12 and 0.15 <= b["attach_t"] <= 0.8]
    for b in branches:
        b["class"] = "minor"
    wings = major_br[:2]

    # Regions: each mask pixel takes the class of its nearest skeleton pixel
    cls = np.zeros(sk.shape, np.int32)
    names = [r[0] for r in SPINE_REGIONS] + ["wing_near", "wing_far", "other"]
    code = {n: i + 1 for i, n in enumerate(names)}
    lo = 0.0
    for name, hi in SPINE_REGIONS:
        sel = (tpar >= lo) & (tpar < hi)
        cls[sp[sel, 0], sp[sel, 1]] = code[name]
        lo = hi
    for b in branches:
        t = b["attach_t"]
        reg = next(n for n, hi in SPINE_REGIONS if t < hi)
        cls[b["_pixels"]] = code[reg]
    # the wings: the larger-area wing is the near one
    seed = cls > 0
    _, lbl = cv2.distanceTransformWithLabels((~seed).astype(np.uint8), cv2.DIST_L2, 5, labelType=cv2.DIST_LABEL_PIXEL)
    seed_pts = np.argwhere(seed)
    # map label -> class: labels are numbered in scan order of zero pixels
    order = np.zeros(lbl.max() + 1, np.int32)
    zy, zx = np.where(seed)
    lbl_at = lbl[zy, zx]
    order[lbl_at] = cls[zy, zx]
    if wings:
        areas = []
        for b in wings:
            tmp = cls.copy()
            tmp[b["_pixels"]] = 99
            o2 = np.zeros_like(order)
            o2[lbl_at] = tmp[zy, zx]
            areas.append(int(((o2[lbl] == 99) & m).sum()))
        order_w = np.argsort(areas)[::-1]
        for rank, wi in enumerate(order_w):
            b = wings[wi]
            b["class"] = "wing_near" if rank == 0 else "wing_far"
            b["area_px"] = areas[wi]
            cls[b["_pixels"]] = code[b["class"]]
        order[lbl_at] = cls[zy, zx]
    regions = np.where(m, order[lbl], 0)

    return {
        "how_ends_found": how,
        "spine": {"length_px": L, "points_px": sxy[:: max(1, len(sxy) // 200)], "head_px": sxy[0], "tail_px": sxy[-1]},
        "radius_profile": prof,
        "branches": [{k: v for k, v in b.items() if not k.startswith("_")} for b in branches],
        "axes": axes,
        "_skeleton": sk, "_dist": dist, "_regions": regions, "_region_names": names,
        "_spine_xy": sxy, "_spine_t": tpar, "_spine_r": rad,
    }


def summary(an):
    """The numbers the comparison and the work order use."""
    L = an["spine"]["length_px"]
    prof = an["radius_profile"]
    out = {
        "spine_length_px": L,
        "radius_of_spine_at_t": {f"{p['t']:.2f}": p["radius_of_spine"] for p in prof},
        "max_radius_of_spine": max(p["radius_of_spine"] for p in prof if p["radius_of_spine"] == p["radius_of_spine"]),
        "wings": {},
        "minor_branch_count": sum(1 for b in an["branches"] if b["class"] == "minor"),
        "major_axis_deg_image": an["axes"]["major_axis_deg_image"],
        "elongation": an["axes"]["elongation"],
        "spine_chord_deg_image": math.degrees(math.atan2(an["spine"]["tail_px"][1] - an["spine"]["head_px"][1],
                                                          an["spine"]["tail_px"][0] - an["spine"]["head_px"][0])),
    }
    for b in an["branches"]:
        if b["class"].startswith("wing"):
            out["wings"][b["class"]] = {
                "attach_t": b["attach_t"], "length_of_spine": b["length_of_spine"],
                "angle_to_spine_deg": b["angle_to_spine_deg"], "mean_radius_of_spine": b["mean_radius_px"] / L,
                "area_of_spine2": b.get("area_px", 0) / (L * L),
            }
    return out


def draw_skeleton(bgr, mask, an):
    out = (bgr.astype(np.float32) * 0.4).astype(np.uint8)
    cnts, _ = cv2.findContours((mask > 0).astype(np.uint8), cv2.RETR_EXTERNAL, cv2.CHAIN_APPROX_NONE)
    cv2.drawContours(out, cnts, -1, (90, 90, 90), 1, cv2.LINE_AA)
    sk = an["_skeleton"]
    out[sk] = (120, 120, 120)
    from common import REGION_COLOURS, put_label
    for b in an["branches"]:
        col = REGION_COLOURS.get(b["class"], (200, 200, 200)) if b["class"] != "minor" else (150, 150, 150)
        p = np.asarray(b["path_px"], np.int32)
        if len(p) > 1:
            cv2.polylines(out, [p], False, col, 2 if b["class"] != "minor" else 1, cv2.LINE_AA)
        if b["class"] != "minor":
            put_label(out, f"{b['class']} {b['length_of_spine']:.2f}L {b['angle_to_spine_deg']:.0f}deg",
                      tuple(np.round(b["tip_px"]).astype(int) + np.array([5, 0])), 0.6, col)
    sxy, sr = an["_spine_xy"], an["_spine_r"]
    for i in range(0, len(sxy), max(1, len(sxy) // 60)):
        cv2.circle(out, tuple(np.round(sxy[i]).astype(int)), int(sr[i]), (0, 160, 255), 1, cv2.LINE_AA)
    cv2.polylines(out, [np.round(sxy).astype(np.int32)], False, (0, 255, 255), 3, cv2.LINE_AA)
    L = an["spine"]["length_px"]
    for p in an["radius_profile"][::2]:
        put_label(out, f"t{p['t']:.1f} r{p['radius_of_spine']:.3f}", tuple(np.round(p["at_px"]).astype(int) + np.array([8, -8])), 0.5, (0, 255, 255))
    put_label(out, "HEAD", tuple(np.round(an["spine"]["head_px"]).astype(int)), 0.7)
    put_label(out, "TAIL", tuple(np.round(an["spine"]["tail_px"]).astype(int)), 0.7)
    put_label(out, f"spine {L:.0f}px; r = inscribed radius / spine length; circles = inscribed discs", (12, 26), 0.7)
    return out


def draw_regions(bgr, an):
    from common import REGION_COLOURS, put_label
    out = (bgr.astype(np.float32) * 0.5).astype(np.uint8)
    reg = an["_regions"]
    for i, name in enumerate(an["_region_names"]):
        sel = reg == i + 1
        if sel.any():
            col = np.array(REGION_COLOURS[name], np.float32)
            out[sel] = (out[sel] * 0.5 + col * 0.5).astype(np.uint8)
            ys, xs = np.where(sel)
            put_label(out, name, (int(np.median(xs)), int(np.median(ys))), 0.7)
    return out
