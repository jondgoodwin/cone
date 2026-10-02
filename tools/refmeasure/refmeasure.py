"""refmeasure: measure a reference picture, and score a render against it.

  python refmeasure.py sheet <image> <out-folder> [--hint hint.json]
  python refmeasure.py compare <sheet.json> <render.bmp> <render-mask.bmp> <out-folder> [--hint hint.json | --camera ARG]
  python refmeasure.py fitcamera <sheet.json> <out-folder> [--exe starship.exe] [--minutes 20]
  python refmeasure.py render <out-prefix> --camera ARG [--size WxH] [--fine]

See README.md.
"""

import argparse
import glob
import math
import os
import subprocess
import sys
import time

import cv2
import numpy as np
from skimage.metrics import structural_similarity

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import camera as cam  # noqa: E402
import colour  # noqa: E402
import outline as outl  # noqa: E402
import skeleton  # noqa: E402
import texture  # noqa: E402
import zoom  # noqa: E402
from common import (REGION_COLOURS, bgr_to_lab, dim, ensure_dir, find_photo_crop, load_bgr, put_label,  # noqa: E402
                    read_json, save_png, write_json)
from mask import mask_overlay, render_mask, subject_mask  # noqa: E402


# ---------------------------------------------------------------- measuring

def measure(bgr, mask, hint=None, palette_k=6):
    """Everything measurable from one picture and its subject mask."""
    lab = bgr_to_lab(bgr)
    L = lab[..., 0]
    sel = mask > 0
    an = skeleton.analyse(mask, hint)
    summ = skeleton.summary(an)
    regions = an["_regions"]
    names = an["_region_names"]
    sxy, st = an["_spine_xy"], an["_spine_t"]

    def spine_dir(lo, hi):
        s = (st >= lo) & (st <= hi)
        if s.sum() < 2:
            return None
        p = sxy[s]
        return math.degrees(math.atan2(p[-1][1] - p[0][1], p[-1][0] - p[0][0]))

    reg_t = {"head": (0, 0.1), "neck": (0.1, 0.25), "body": (0.25, 0.62), "tail": (0.62, 1.0)}
    palettes = {"subject": colour.palette(lab, sel, palette_k)}
    means = {"subject": colour.mean_lab(lab, sel)}
    tex = {"subject": texture.region_stats(L, sel, spine_dir(0, 1))}
    region_area = {}
    for i, n in enumerate(names):
        rs = regions == i + 1
        region_area[n] = float(rs.sum() / max(sel.sum(), 1))
        if rs.sum() < 400:
            continue
        palettes[n] = colour.palette(lab, rs, 4)
        means[n] = colour.mean_lab(lab, rs)
        lohi = reg_t.get(n)
        if lohi is None:
            att = [b for b in an["branches"] if b["class"] == n]
            lohi = (max(0, att[0]["attach_t"] - 0.1), min(1, att[0]["attach_t"] + 0.1)) if att else (0, 1)
        tex[n] = texture.region_stats(L, rs, spine_dir(*lohi))
    ramps = {"spine_head_to_tail": colour.ramp(lab, mask, sxy, 16, 6)}
    for b in an["branches"]:
        if b["class"].startswith("wing"):
            ramps[b["class"] + "_root_to_tip"] = colour.ramp(lab, mask, b["path_px"], 12, 6)
    outlines = outl.outline(mask)
    return {
        "image_size": [bgr.shape[1], bgr.shape[0]],
        "mask_area_frac": float(sel.mean()),
        "skeleton": summ,
        "skeleton_detail": {k: v for k, v in an.items() if not k.startswith("_")},
        "region_area_frac": region_area,
        "palettes": palettes,
        "mean_lab": means,
        "ramps": ramps,
        "texture": tex,
        "outline": [{"area_px": o["area_px"], "traced_points": o["traced_points"], "segments": len(o["beziers"]),
                     "max_err_px": max(s["max_err_px"] for s in o["beziers"]),
                     "mean_err_px": float(np.mean([s["max_err_px"] for s in o["beziers"]])),
                     "rdp_vertices": len(o["rdp"]), "corners": len(o["corners"]),
                     "beziers": [s["p"] for s in o["beziers"]],
                     "segment_max_err_px": [s["max_err_px"] for s in o["beziers"]]} for o in outlines],
        "_an": an, "_outlines": outlines, "_lab": lab,
    }


def public(m):
    return {k: v for k, v in m.items() if not k.startswith("_")}


# ---------------------------------------------------------------- sheet

def cmd_sheet(a):
    full = load_bgr(a.image)
    crop = find_photo_crop(full)
    bgr = full[crop[1]:crop[3], crop[0]:crop[2]].copy()
    hint = read_json(a.hint) if a.hint else None
    out = ensure_dir(a.out)
    t0 = time.time()
    mask, minfo = subject_mask(bgr, hint)
    t_mask = time.time() - t0
    m = measure(bgr, mask, hint)
    save_png(os.path.join(out, "reference-crop.png"), bgr)
    save_png(os.path.join(out, "mask.png"), mask)
    save_png(os.path.join(out, "overlay-mask.png"), mask_overlay(bgr, mask))
    save_png(os.path.join(out, "overlay-outline.png"), outl.draw_outline(bgr, m["_outlines"]))
    save_png(os.path.join(out, "overlay-skeleton.png"), skeleton.draw_skeleton(bgr, mask, m["_an"]))
    save_png(os.path.join(out, "overlay-regions.png"), skeleton.draw_regions(bgr, m["_an"]))
    save_png(os.path.join(out, "overlay-palette.png"), colour.draw_palettes(m["palettes"], m["ramps"]))
    save_png(os.path.join(out, "overlay-orientation.png"), texture.draw_orientation(bgr, m["_lab"][..., 0], mask > 0))
    sheet = {
        "tool": "refmeasure sheet",
        "source": os.path.abspath(a.image),
        "crop_box_px": crop,
        "crop_note": "all pixel numbers are in the cropped picture (the screenshot's page border removed)",
        "files": {"crop": "reference-crop.png", "mask": "mask.png"},
        "hint": hint,
        "hint_file": os.path.abspath(a.hint) if a.hint else None,
        "mask": minfo,
        "seconds": {"mask": t_mask, "total": time.time() - t0},
        **public(m),
    }
    write_json(os.path.join(out, "sheet.json"), sheet)
    print(f"sheet: {out}  mask {minfo['area_frac']:.3f} of the picture, spine {m['skeleton']['spine_length_px']:.0f}px, "
          f"{time.time() - t0:.1f}s")


# ---------------------------------------------------------------- compare

def chamfer(a_edges, b_edges):
    if not a_edges.any() or not b_edges.any():
        return None
    da = cv2.distanceTransform((~a_edges).astype(np.uint8), cv2.DIST_L2, 5)
    db = cv2.distanceTransform((~b_edges).astype(np.uint8), cv2.DIST_L2, 5)
    ab = float(da[b_edges].mean())   # ours to ref
    ba = float(db[a_edges].mean())   # ref to ours
    return {"ours_to_ref_px": ab, "ref_to_ours_px": ba, "symmetric_px": (ab + ba) / 2}


def outline_edges(mask):
    m = (mask > 0).astype(np.uint8)
    return (m - cv2.erode(m, np.ones((3, 3), np.uint8))) > 0


def interior_edges(bgr, mask):
    L = bgr_to_lab(bgr)[..., 0]
    e = cv2.Canny(np.clip(L * 2.55, 0, 255).astype(np.uint8), 20, 60) > 0
    return e & (cv2.erode((mask > 0).astype(np.uint8), np.ones((5, 5), np.uint8)) > 0)


def iou(a, b):
    a, b = a > 0, b > 0
    u = (a | b).sum()
    return float((a & b).sum() / u) if u else 0.0


def load_reference(sheet_path, size):
    sheet = read_json(sheet_path)
    base = os.path.dirname(os.path.abspath(sheet_path))
    ref = load_bgr(os.path.join(base, sheet["files"]["crop"]))
    rmask = load_bgr(os.path.join(base, sheet["files"]["mask"]))[..., 0]
    sx, sy = size[0] / ref.shape[1], size[1] / ref.shape[0]
    ref = cv2.resize(ref, size, interpolation=cv2.INTER_AREA)
    rmask = (cv2.resize(rmask, size, interpolation=cv2.INTER_AREA) > 127).astype(np.uint8) * 255
    hint = None
    if sheet.get("hint") and "head" in sheet["hint"]:
        hint = {"head": [sheet["hint"]["head"][0] * sx, sheet["hint"]["head"][1] * sy],
                "tail": [sheet["hint"]["tail"][0] * sx, sheet["hint"]["tail"][1] * sy]}
    return sheet, ref, rmask, hint


def render_hint_from_camera(arg, size):
    v = [float(x) for x in arg.split(",")]
    lm = cam.starship_landmarks()
    px = cam.project(np.array([lm["head"], lm["tail"]]), v[0:3], v[3:6], v[6:9], v[9], size[0], size[1])
    return {"head": px[0].tolist(), "tail": px[1].tolist()}


def diff(a, b):
    try:
        return None if a is None or b is None else b - a
    except TypeError:
        return None


def discrepancies(score):
    """The measured differences, worst first. Severity is a heuristic that
    puts unlike units on one scale: a ratio's distance from 1 (ours over
    the reference; 0.5 and 2 both count 1), an angle's difference over 30
    degrees, a CIEDE2000 over 10, a texture difference over its typical spread."""
    out = []

    def ratio(name, ref, ours, unit=""):
        if ref is None or ours is None or ref == 0 or ours == 0:
            return
        r = ours / ref
        out.append({"what": name, "ref": ref, "ours": ours, "ours_over_ref": r, "unit": unit,
                    "severity": abs(math.log2(abs(r)))})

    def delta(name, ref, ours, per, unit):
        if ref is None or ours is None:
            return
        d = ours - ref
        if unit == "deg":
            d = (d + 180) % 360 - 180
        out.append({"what": name, "ref": ref, "ours": ours, "diff": d, "unit": unit, "severity": abs(d) / per})

    sk = score["skeleton"]
    prof = {p["t"]: p for p in sk["radius_profile_of_spine"]}
    for lo, hi, name in ((0.0, 0.1, "head"), (0.1, 0.3, "neck"), (0.3, 0.6, "chest (wing roots inflate both)"),
                         (0.6, 0.85, "tail root"), (0.85, 1.0, "tail tip")):
        sel = [p for t, p in prof.items() if lo <= t < hi and p["ours"] is not None]
        if sel:
            ratio(f"body radius / spine length, {name} (t {lo}-{hi})", float(np.mean([p["ref"] for p in sel])),
                  float(np.mean([p["ours"] for p in sel])))
    delta("spine direction head-to-tail in the image", sk["spine_chord_deg_image"]["ref"], sk["spine_chord_deg_image"]["ours"], 30, "deg")
    for w in ("wing_near", "wing_far"):
        r, o = sk["wings"][w]["ref"], sk["wings"][w]["ours"]
        if r and o:
            ratio(f"{w} length / spine length", r["length_of_spine"], o["length_of_spine"])
            delta(f"{w} angle to the spine (0 = along it towards the tail)", r["angle_to_spine_deg"], o["angle_to_spine_deg"], 30, "deg")
            ratio(f"{w} area / spine length squared", r["area_of_spine2"], o["area_of_spine2"])
            delta(f"{w} attachment along the spine (t)", r["attach_t"], o["attach_t"], 0.15, "t")
        elif r and not o:
            out.append({"what": f"{w}: found in the reference, not in ours", "severity": 2.0})
    ratio("silhouette area, ours over reference", 1.0, score["area_ratio_ours_over_ref"])
    ratio("silhouette elongation (moments)", sk["elongation"]["ref"], sk["elongation"]["ours"])
    ide = score["interior_edge_density"]
    ratio("interior edge density (surface detail)", ide["ref"], ide["ours"])
    for n, c in score["colour"].items():
        if c["mean_colour_dE00"] is not None:
            out.append({"what": f"mean colour, {n} (CIEDE2000)", "ref_lab": c["mean_lab_ref"], "ours_lab": c["mean_lab_ours"],
                        "dE00": c["mean_colour_dE00"], "severity": c["mean_colour_dE00"] / 10})
    for n, t in score["texture"].items():
        delta(f"texture {n}: spectral slope beta (higher = smoother)", t["spectral_slope_beta"]["ref"], t["spectral_slope_beta"]["ours"], 0.5, "")
        delta(f"texture {n}: grain coherence", t["coherence_local"]["ref"], t["coherence_local"]["ours"], 0.25, "")
        ratio(f"texture {n}: edge density", t["edge_density"]["ref"], t["edge_density"]["ours"])
        delta(f"texture {n}: grain angle to the spine", t["grain_deg_to_spine"]["ref"], t["grain_deg_to_spine"]["ours"], 45, "deg")
    out.sort(key=lambda d: -d["severity"])
    return out


def cmd_compare(a):
    ours = load_bgr(a.render)
    omask = render_mask(a.mask)
    size = (ours.shape[1], ours.shape[0])
    sheet, ref, rmask, rhint = load_reference(a.sheet, size)
    ohint = read_json(a.hint) if a.hint else (render_hint_from_camera(a.camera, size) if a.camera else None)
    out = ensure_dir(a.out)
    t0 = time.time()
    mr = measure(ref, rmask, rhint)
    mo = measure(ours, omask, ohint)
    score = {"tool": "refmeasure compare", "sheet": os.path.abspath(a.sheet), "render": os.path.abspath(a.render),
             "render_mask": os.path.abspath(a.mask), "size": list(size), "camera": a.camera, "render_hint": ohint,
             "reference_hint_scaled": rhint}
    score["silhouette_iou"] = iou(rmask, omask)
    Lr = mr["skeleton"]["spine_length_px"]
    score["area_ratio_ours_over_ref"] = float((omask > 0).sum() / max((rmask > 0).sum(), 1))
    ch = chamfer(outline_edges(rmask), outline_edges(omask))
    ci = chamfer(interior_edges(ref, rmask), interior_edges(ours, omask))
    score["chamfer_outline"] = ch and {**ch, "symmetric_of_ref_spine": ch["symmetric_px"] / Lr}
    score["chamfer_interior_edges"] = ci and {**ci, "symmetric_of_ref_spine": ci["symmetric_px"] / Lr}
    score["interior_edge_density"] = {"ref": float(interior_edges(ref, rmask).sum() / max((rmask > 0).sum(), 1)),
                                      "ours": float(interior_edges(ours, omask).sum() / max((omask > 0).sum(), 1))}

    # skeleton and radii
    sr, so = mr["skeleton"], mo["skeleton"]
    prof = []
    for t, rr in sr["radius_of_spine_at_t"].items():
        ro = so["radius_of_spine_at_t"].get(t)
        prof.append({"t": float(t), "ref": rr, "ours": ro, "ours_over_ref": (ro / rr) if (ro and rr) else None})
    score["skeleton"] = {
        "spine_length_px": {"ref": sr["spine_length_px"], "ours": so["spine_length_px"]},
        "spine_chord_deg_image": {"ref": sr["spine_chord_deg_image"], "ours": so["spine_chord_deg_image"]},
        "radius_profile_of_spine": prof,
        "radius_profile_mean_abs_diff_of_spine": float(np.nanmean([abs(p["ours"] - p["ref"]) for p in prof if p["ours"] is not None])),
        "max_radius_of_spine": {"ref": sr["max_radius_of_spine"], "ours": so["max_radius_of_spine"]},
        "wings": {k: {"ref": sr["wings"].get(k), "ours": so["wings"].get(k)} for k in ("wing_near", "wing_far")},
        "elongation": {"ref": sr["elongation"], "ours": so["elongation"]},
    }
    # colour
    col = {}
    for n in mr["palettes"]:
        if n in mo["palettes"]:
            pd = colour.palette_delta(mr["palettes"][n], mo["palettes"][n])
            col[n] = {"palette_weighted_dE00": pd and pd["weighted_dE00"],
                      "mean_colour_dE00": colour.de00(mr["mean_lab"][n], mo["mean_lab"][n]),
                      "mean_lab_ref": mr["mean_lab"][n], "mean_lab_ours": mo["mean_lab"][n]}
    score["colour"] = col
    # texture
    tx = {}
    keys = ["spectral_slope_beta", "hurst_H_unclipped", "grain_deg_to_spine", "coherence_local", "coherence_region",
            "gabor_wavelength_px", "spectral_centroid_wavelength_px", "edge_density", "L_mean", "L_std"]
    for n, tr in mr["texture"].items():
        to = mo["texture"].get(n)
        if tr and to:
            tx[n] = {k: {"ref": tr[k], "ours": to[k], "diff": diff(tr[k], to[k])} for k in keys}
    score["texture"] = tx
    score["region_area_frac"] = {"ref": mr["region_area_frac"], "ours": mo["region_area_frac"]}

    # tile SSIM on lightness
    gr = cv2.cvtColor(ref, cv2.COLOR_BGR2GRAY)
    go = cv2.cvtColor(ours, cv2.COLOR_BGR2GRAY)
    _, smap = structural_similarity(gr, go, full=True, data_range=255, gaussian_weights=True, sigma=1.5)
    T = a.tile
    tiles = []
    union = (rmask > 0) | (omask > 0)
    for y in range(0, size[1] - T + 1, T):
        for x in range(0, size[0] - T + 1, T):
            cover = union[y:y + T, x:x + T].mean()
            if cover < 0.1:
                continue
            tiles.append({"x": x, "y": y, "size": T, "ssim": float(smap[y:y + T, x:x + T].mean()), "subject_cover": float(cover)})
    tiles.sort(key=lambda d: d["ssim"])
    score["ssim_tiles"] = {"tile_px": T, "count": len(tiles), "mean_ssim": float(np.mean([t["ssim"] for t in tiles])) if tiles else None,
                           "worst": tiles[:8]}
    score["discrepancies"] = discrepancies(score)
    score["seconds"] = time.time() - t0

    # overlays
    ov = dim(ref, 0.6)
    for msk, colr in ((rmask, (0, 255, 0)), (omask, (255, 0, 255))):
        cnts, _ = cv2.findContours((msk > 0).astype(np.uint8), cv2.RETR_EXTERNAL, cv2.CHAIN_APPROX_NONE)
        cv2.drawContours(ov, cnts, -1, colr, 2, cv2.LINE_AA)
    for anx, colr in ((mr["_an"], (0, 200, 0)), (mo["_an"], (200, 0, 200))):
        cv2.polylines(ov, [np.round(anx["_spine_xy"]).astype(np.int32)], False, colr, 2, cv2.LINE_AA)
        for b in anx["branches"]:
            if b["class"].startswith("wing"):
                cv2.polylines(ov, [np.asarray(b["path_px"], np.int32)], False, colr, 2, cv2.LINE_AA)
    put_label(ov, f"green: reference outline + spine/wings; magenta: ours.  IoU {score['silhouette_iou']:.3f}", (12, 26), 0.6)
    save_png(os.path.join(out, "compare-outlines.png"), ov)
    heat = np.clip((1 - smap) * 255, 0, 255).astype(np.uint8)
    heat = cv2.applyColorMap(heat, cv2.COLORMAP_INFERNO)
    hv = cv2.addWeighted(dim(ref, 0.8), 0.45, heat, 0.55, 0)
    for i, t in enumerate(tiles[:8]):
        cv2.rectangle(hv, (t["x"], t["y"]), (t["x"] + T, t["y"] + T), (255, 255, 255), 2)
        put_label(hv, f"{i + 1}: {t['ssim']:.2f}", (t["x"] + 4, t["y"] + 18), 0.5)
    put_label(hv, "1 - SSIM (bright = unlike); boxes = worst tiles", (12, 26), 0.6)
    save_png(os.path.join(out, "compare-ssim-heat.png"), hv)
    side = np.hstack([ref, ours])
    save_png(os.path.join(out, "compare-side-by-side.png"), side)
    # worst tiles cut out, enlarged, reference above ours
    cuts = []
    Z = 3
    for i, t in enumerate(tiles[:6]):
        pad = T // 2
        x0, y0 = max(0, t["x"] - pad), max(0, t["y"] - pad)
        x1, y1 = min(size[0], t["x"] + T + pad), min(size[1], t["y"] + T + pad)
        r = cv2.resize(ref[y0:y1, x0:x1], None, fx=Z, fy=Z, interpolation=cv2.INTER_CUBIC)
        o = cv2.resize(ours[y0:y1, x0:x1], None, fx=Z, fy=Z, interpolation=cv2.INTER_CUBIC)
        pair = np.vstack([r, np.full((6, r.shape[1], 3), 255, np.uint8), o])
        put_label(pair, f"#{i + 1} ssim {t['ssim']:.2f} at ({t['x']},{t['y']})", (6, 20), 0.55)
        cuts.append(cv2.copyMakeBorder(pair, 0, 0, 0, 8, cv2.BORDER_CONSTANT, value=(255, 255, 255)))
    if cuts:
        hmax = max(c.shape[0] for c in cuts)
        cuts = [cv2.copyMakeBorder(c, 0, hmax - c.shape[0], 0, 0, cv2.BORDER_CONSTANT, value=(0, 0, 0)) for c in cuts]
        save_png(os.path.join(out, "compare-worst-tiles.png"), np.hstack(cuts))
    save_png(os.path.join(out, "compare-skeleton-ref.png"), skeleton.draw_skeleton(ref, rmask, mr["_an"]))
    save_png(os.path.join(out, "compare-skeleton-ours.png"), skeleton.draw_skeleton(ours, omask, mo["_an"]))
    save_png(os.path.join(out, "compare-regions-ours.png"), skeleton.draw_regions(ours, mo["_an"]))
    score["measured"] = {"ref": public(mr), "ours": public(mo)}
    write_json(os.path.join(out, a.name), score)
    print(f"compare: IoU {score['silhouette_iou']:.3f}, outline chamfer {ch and ch['symmetric_px']:.1f}px, "
          f"mean tile SSIM {score['ssim_tiles']['mean_ssim']:.3f}  -> {out}")


# ---------------------------------------------------------------- rendering

def default_exe():
    hits = sorted(glob.glob(os.path.expanduser(r"~\.congo\lone\starship-*\release\starship.exe")), key=os.path.getmtime)
    return hits[-1] if hits else None


def run_starship(exe, prefix, cams, size, coarse=True, sdl_dir=None):
    env = dict(os.environ)
    sdl_dir = sdl_dir or os.environ.get("SDL3_DIR") or r"C:\libs\SDL3-3.4.16\lib\x64"
    if os.path.isdir(sdl_dir):
        env["PATH"] = sdl_dir + os.pathsep + env.get("PATH", "")
    args = [exe, "--mask", "--size", f"{size[0]}x{size[1]}", "--shots", prefix]
    if coarse:
        args.append("--coarse")
    for c in cams:
        args += ["--camera", c]
    r = subprocess.run(args, env=env, capture_output=True, text=True)
    if r.returncode != 0:
        raise RuntimeError(f"starship failed ({r.returncode}):\n{r.stdout[-2000:]}\n{r.stderr[-2000:]}")
    return [(f"{prefix}-cam{i}.bmp", f"{prefix}-cam{i}-mask.bmp") for i in range(len(cams))]


def cmd_render(a):
    exe = a.exe or default_exe()
    size = tuple(int(v) for v in a.size.split("x"))
    t0 = time.time()
    shots = run_starship(exe, a.prefix, [a.camera], size, coarse=not a.fine)
    print(f"rendered {shots[0]} in {time.time() - t0:.1f}s")


# ---------------------------------------------------------------- camera fit

def cmd_fitcamera(a):
    import cma
    exe = a.exe or default_exe()
    if not exe:
        raise SystemExit("no starship.exe; build it with congo first or pass --exe")
    out = ensure_dir(a.out)
    tmp = ensure_dir(os.path.join(out, "fit-tmp"))
    W = a.width
    sheet = read_json(a.sheet)
    rw, rh = sheet["crop_box_px"][2] - sheet["crop_box_px"][0], sheet["crop_box_px"][3] - sheet["crop_box_px"][1]
    H = int(round(W * rh / rw))
    _, _, rmask, rhint = load_reference(a.sheet, (W, H))
    rm = rmask > 0
    # Silhouette IoU alone cannot tell head from tail: an IoU-only search
    # found its best overlap with the ship reversed. So the objective also
    # asks the head's and tail's projections to land on the reference's
    # (from the sheet's hint): IoU less 'landmark_weight' times their mean
    # distance as a fraction of the picture's diagonal.
    lm = cam.starship_landmarks()
    lm_pts = np.array([lm["head"], lm["tail"]])
    diag = math.hypot(W, H)
    lw = a.landmark_weight if rhint else 0.0

    log = []
    t_start = time.time()
    budget = a.minutes * 60

    def evaluate(xs, tag):
        cams, lms = [], []
        for x in xs:
            e, t, u, f = cam.from_params(x)
            cams.append(cam.camera_arg(e, t, u, f))
            if rhint:
                px = cam.project(lm_pts, e, t, u, f, W, H)
                lms.append(float((np.linalg.norm(px[0] - rhint["head"]) + np.linalg.norm(px[1] - rhint["tail"])) / 2 / diag))
            else:
                lms.append(0.0)
        t0 = time.time()
        shots = run_starship(exe, os.path.join(tmp, tag), cams, (W, H), coarse=True)
        dt = time.time() - t0
        res = []
        for (_, mp), le in zip(shots, lms):
            v = iou(rm, render_mask(mp) > 0)
            res.append((v - lw * le, v, le))
        return res, dt

    hero_x = cam.to_params(*cam.HERO)
    starts = [("hero", hero_x)]
    below = hero_x.copy()
    below[5] = -abs(below[5])          # the same, from below
    starts.append(("below", below))
    other = below.copy()
    other[4] += 180                    # from below, the other side
    starts.append(("below-other-side", other))
    if a.start:
        # Given cameras replace the default starts; each also starts mirrored
        # below (its elevation negated), to settle above against below
        starts = []
        for i, arg in enumerate(a.start):
            v = [float(t) for t in arg.split(",")]
            x = cam.to_params(v[0:3], v[3:6], v[6:9], v[9])
            b = x.copy()
            b[5] = -b[5]
            starts += [(f"start{i}", x), (f"start{i}-below", b)]
        hero_x = starts[0][1]
    # bounds: target within the ship's box, distance 5..80, fov 15..70
    lo = np.array([-2, -3, -6, math.log(5), -360, -89, -180, 15])
    hi = np.array([14, 4, 6, math.log(80), 360, 89, 180, 70])
    scale = np.array([2, 1, 2, 0.3, 30, 20, 30, 8])
    hero_res, dt0 = evaluate([hero_x], "hero")
    hero_iou = hero_res[0][1]
    print(f"hero camera IoU {hero_iou:.3f}, landmark error {hero_res[0][2]:.3f} of the diagonal ({dt0:.1f}s for one render incl. meshing)")
    best = (hero_res[0], hero_x, "hero")
    per_start = budget / len(starts)
    timing = []
    for name, x0 in starts:
        es = cma.CMAEvolutionStrategy(list((x0 - x0) / scale), 1.0,
                                      {"popsize": a.popsize, "seed": 7, "verbose": -9,
                                       "bounds": [list((lo - x0) / scale), list((hi - x0) / scale)]})
        t_s = time.time()
        gen = 0
        while not es.stop() and time.time() - t_s < per_start:
            zs = es.ask()
            xs = [x0 + np.asarray(z) * scale for z in zs]
            res, dt = evaluate(xs, f"{name}-g")
            timing.append(dt)
            es.tell(zs, [-r[0] for r in res])
            i = int(np.argmax([r[0] for r in res]))
            if res[i][0] > best[0][0]:
                best = (res[i], xs[i], name)
            log.append({"start": name, "gen": gen, "best_objective_gen": res[i][0], "iou": res[i][1], "landmark_err": res[i][2],
                        "best_objective": best[0][0], "seconds": dt})
            if gen % 5 == 0:
                print(f"  {name} gen {gen}: this gen IoU {res[i][1]:.3f} landmarks {res[i][2]:.3f}; "
                      f"overall IoU {best[0][1]:.3f} landmarks {best[0][2]:.3f} ({dt:.1f}s)")
            gen += 1
    e, t, u, f = cam.from_params(best[1])
    result = {
        "reference_sheet": os.path.abspath(a.sheet),
        "fit_size": [W, H],
        "objective": f"IoU - {lw} x mean head/tail landmark distance / picture diagonal",
        "iou_before_hero_camera": hero_iou,
        "landmark_err_before": hero_res[0][2],
        "hero_camera": cam.camera_dict(*cam.HERO),
        "iou_after": best[0][1],
        "landmark_err_after": best[0][2],
        "start_that_won": best[2],
        "camera": cam.camera_dict(e, t, u, f),
        "params": {"target": best[1][:3], "distance": math.exp(best[1][3]), "azimuth_deg": best[1][4],
                   "elevation_deg": best[1][5], "roll_deg": best[1][6], "fov_deg": best[1][7]},
        "evaluations": len(log) * a.popsize + 1,
        "generations": len(log),
        "seconds_total": time.time() - t_start,
        "seconds_per_generation_mean": float(np.mean(timing)) if timing else None,
        "renders_per_generation": a.popsize,
        "log": log,
    }
    write_json(os.path.join(out, "fitcamera.json"), result)
    print(f"fitted: IoU {hero_iou:.3f} -> {best[0][1]:.3f}, landmarks {hero_res[0][2]:.3f} -> {best[0][2]:.3f}; "
          f"camera {result['camera']['arg']}")


def main():
    p = argparse.ArgumentParser(prog="refmeasure")
    sub = p.add_subparsers(dest="cmd", required=True)
    s = sub.add_parser("sheet")
    s.add_argument("image"); s.add_argument("out"); s.add_argument("--hint")
    s.set_defaults(fn=cmd_sheet)
    c = sub.add_parser("compare")
    c.add_argument("sheet"); c.add_argument("render"); c.add_argument("mask"); c.add_argument("out")
    c.add_argument("--hint"); c.add_argument("--camera", help="the render's camera, to place its head and tail")
    c.add_argument("--tile", type=int, default=64); c.add_argument("--name", default="score.json")
    c.set_defaults(fn=cmd_compare)
    f = sub.add_parser("fitcamera")
    f.add_argument("sheet"); f.add_argument("out"); f.add_argument("--exe")
    f.add_argument("--minutes", type=float, default=20); f.add_argument("--width", type=int, default=384)
    f.add_argument("--popsize", type=int, default=16)
    f.add_argument("--landmark-weight", type=float, default=1.0)
    f.add_argument("--start", action="append",
                   help="a camera ARG to start from, as --start=ARG (repeatable); replaces the default starts")
    f.set_defaults(fn=cmd_fitcamera)
    r = sub.add_parser("render")
    r.add_argument("prefix"); r.add_argument("--camera", required=True); r.add_argument("--size", default="1280x1280")
    r.add_argument("--fine", action="store_true"); r.add_argument("--exe")
    r.set_defaults(fn=cmd_render)
    zoom.register(sub)
    a = p.parse_args()
    a.fn(a)


if __name__ == "__main__":
    main()
