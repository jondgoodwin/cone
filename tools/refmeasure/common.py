"""Shared helpers: loading, cropping, colour conversion, JSON, drawing."""

import json
import os

import cv2
import numpy as np
from skimage import color as skcolor


def load_bgr(path):
    img = cv2.imdecode(np.fromfile(path, dtype=np.uint8), cv2.IMREAD_COLOR)
    if img is None:
        raise SystemExit(f"cannot read image {path}")
    return img


def save_png(path, img):
    ok, buf = cv2.imencode(".png", img)
    if not ok:
        raise SystemExit(f"cannot encode {path}")
    buf.tofile(path)


def find_photo_crop(bgr, white=235, fill=0.9):
    """The picture inside a screenshot: the band of rows (then columns) that
    are mostly not the near-white page around it. The whole image when
    there is no such border."""
    g = cv2.cvtColor(bgr, cv2.COLOR_BGR2GRAY)
    rows = np.where((g < white).mean(1) > fill)[0]
    if len(rows) < 0.2 * g.shape[0]:
        return [0, 0, g.shape[1], g.shape[0]]
    # the longest run of consecutive rows
    runs = np.split(rows, np.where(np.diff(rows) != 1)[0] + 1)
    run = max(runs, key=len)
    y0, y1 = int(run[0]), int(run[-1]) + 1
    cols = np.where((g[y0:y1] < white).mean(0) > fill)[0]
    runs = np.split(cols, np.where(np.diff(cols) != 1)[0] + 1)
    run = max(runs, key=len)
    x0, x1 = int(run[0]), int(run[-1]) + 1
    return [x0, y0, x1, y1]


def bgr_to_lab(bgr):
    """CIE L*a*b* (L 0..100) as float64, D65."""
    rgb = cv2.cvtColor(bgr, cv2.COLOR_BGR2RGB).astype(np.float64) / 255.0
    return skcolor.rgb2lab(rgb)


def lab_to_bgr8(lab):
    lab = np.asarray(lab, dtype=np.float64).reshape(-1, 1, 3)
    rgb = np.clip(skcolor.lab2rgb(lab), 0, 1)
    return (rgb[:, 0, ::-1] * 255 + 0.5).astype(np.uint8)


def to_jsonable(x):
    if isinstance(x, dict):
        return {str(k): to_jsonable(v) for k, v in x.items()}
    if isinstance(x, (list, tuple)):
        return [to_jsonable(v) for v in x]
    if isinstance(x, np.ndarray):
        return to_jsonable(x.tolist())
    if isinstance(x, (np.floating, float)):
        v = float(x)
        return None if not np.isfinite(v) else round(v, 5)
    if isinstance(x, (np.integer,)):
        return int(x)
    if isinstance(x, np.bool_):
        return bool(x)
    return x


def write_json(path, obj):
    with open(path, "w", encoding="utf-8") as f:
        json.dump(to_jsonable(obj), f, indent=1)


def read_json(path):
    with open(path, encoding="utf-8") as f:
        return json.load(f)


def dim(bgr, factor=0.45):
    return (bgr.astype(np.float32) * factor).astype(np.uint8)


def put_label(img, text, org, scale=0.6, col=(255, 255, 255)):
    cv2.putText(img, text, org, cv2.FONT_HERSHEY_SIMPLEX, scale, (0, 0, 0), 4, cv2.LINE_AA)
    cv2.putText(img, text, org, cv2.FONT_HERSHEY_SIMPLEX, scale, col, 1, cv2.LINE_AA)


def ensure_dir(d):
    os.makedirs(d, exist_ok=True)
    return d


# Region colours for overlays (BGR)
REGION_COLOURS = {
    "head": (60, 200, 255),
    "neck": (0, 140, 255),
    "body": (80, 220, 80),
    "tail": (255, 160, 60),
    "wing_near": (220, 80, 220),
    "wing_far": (255, 255, 0),
    "other": (160, 160, 160),
}
