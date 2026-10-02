"""The render's camera, as the `render` package builds it (geomath's lookAt
and Mat4.perspective: right-handed, looking down -z, vertical field of view),
its parameterisation for the search, and the starship's own landmarks.

Parameters for the search, as a vector:
  target x, y, z; log distance; azimuth and elevation of the eye about the
  target (degrees; elevation positive above); roll about the view axis
  (degrees); vertical field of view (degrees).
"""

import math

import numpy as np


def look_at(eye, target, up):
    eye, target, up = (np.asarray(v, float) for v in (eye, target, up))
    fwd = target - eye
    fwd /= np.linalg.norm(fwd)
    side = np.cross(fwd, up)
    side /= np.linalg.norm(side)
    nup = np.cross(side, fwd)
    return fwd, side, nup


def project(points, eye, target, up, fov_deg, width, height):
    """World points to pixel (x right, y down)."""
    fwd, side, nup = look_at(eye, target, up)
    p = np.asarray(points, float) - np.asarray(eye, float)
    xc, yc, zc = p @ side, p @ nup, p @ fwd
    f = 1 / math.tan(math.radians(fov_deg) / 2)
    aspect = width / height
    nx = f / aspect * xc / zc
    ny = f * yc / zc
    return np.stack([(nx + 1) / 2 * width, (1 - ny) / 2 * height], 1)


def from_params(x):
    tx, ty, tz, logd, az, el, roll, fov = x
    d = math.exp(logd)
    az, el = math.radians(az), math.radians(el)
    target = np.array([tx, ty, tz])
    eye = target + d * np.array([math.cos(el) * math.cos(az), math.sin(el), math.cos(el) * math.sin(az)])
    fwd = (target - eye) / d
    world_up = np.array([0.0, 1.0, 0.0]) if abs(math.cos(el)) > 1e-3 else np.array([1.0, 0.0, 0.0])
    side = np.cross(fwd, world_up)
    side /= np.linalg.norm(side)
    nup = np.cross(side, fwd)
    r = math.radians(roll)
    up = math.cos(r) * nup + math.sin(r) * side
    return eye, target, up, fov


def to_params(eye, target, up, fov):
    eye, target, up = (np.asarray(v, float) for v in (eye, target, up))
    off = eye - target
    d = np.linalg.norm(off)
    el = math.degrees(math.asin(off[1] / d))
    az = math.degrees(math.atan2(off[2], off[0]))
    base = from_params([*target, math.log(d), az, el, 0.0, fov])
    fwd, side, nup = look_at(eye, target, up)
    _, side0, nup0 = look_at(base[0], base[1], base[2])
    roll = math.degrees(math.atan2(nup @ side0, nup @ nup0))
    return np.array([*target, math.log(d), az, el, roll, fov])


def camera_arg(eye, target, up, fov):
    return ",".join(f"{v:.5f}" for v in [*eye, *target, *up, fov])


def camera_dict(eye, target, up, fov):
    return {"eye": list(map(float, eye)), "target": list(map(float, target)), "up": list(map(float, up)), "fov_deg": float(fov),
            "arg": camera_arg(eye, target, up, fov)}


# starship.cone's hero shot
HERO = (np.array([-3.0, 12.0, 12.5]), np.array([6.0, -0.3, 2.0]), np.array([0.6, 1.0, 0.0]), 35.0)


# starship.cone's line of action: its SPX and SPY control points (Catmull-Rom
# through them; the body runs from point 1 to point 9, the lance on to 11)
STARSHIP_SPX = [-0.73, -0.08, 0.72, 1.6, 2.65, 3.77, 5.28, 7.14, 8.88, 10.6, 11.94, 13.31]
STARSHIP_SPY = [-0.26, -0.03, 0.41, 1.16, 1.8, 1.95, 1.53, 0.98, 0.67, 0.66, 0.91, 1.17]


def _starship_spine_at(t):
    """starship.cone's spineAt: the body's spine at t in [0, 1]."""
    s = t * 8.0
    k = min(int(s), 7)
    a, b, c, d = (np.array([STARSHIP_SPX[k + i], STARSHIP_SPY[k + i], 0.0]) for i in range(4))
    u = s - k
    return 0.5 * (2 * b + (c - a) * u + (2 * a - 5 * b + 4 * c - d) * u * u + (3 * b - a - 3 * c + d) * u ** 3)


def starship_landmarks():
    """The head's tip and the tail's, from starship.cone's own numbers: the
    snout's control point less its tip radius, and the lance's point."""
    head = np.array([STARSHIP_SPX[0] - 0.11, STARSHIP_SPY[0], 0.0])
    tail = np.array([STARSHIP_SPX[11], STARSHIP_SPY[11], 0.0])
    return {"head": head, "tail": tail}


def starship_spine(t0=0.0, t1=1.0, n=200, lift=0.0):
    """Points along starship.cone's spine (the same curve as its spineAt),
    from t0 to t1, raised `lift` units in y (towards the back), and the
    cumulative 3-D arc length at each."""
    pts = np.array([_starship_spine_at(t) for t in np.linspace(t0, t1, n)])
    pts[:, 1] += lift
    arc = np.concatenate([[0.0], np.cumsum(np.linalg.norm(np.diff(pts, axis=0), axis=1))])
    return pts, arc


def parse_arg(arg):
    v = [float(x) for x in arg.split(",")]
    return np.array(v[0:3]), np.array(v[3:6]), np.array(v[6:9]), v[9]
