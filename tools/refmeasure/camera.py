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


def starship_landmarks():
    """The head's tip and the tail's, from starship.cone's own numbers."""
    def spine_at(t):
        arch = (0.55 + 0.5 * (1 - t) ** 2) * math.sin(3.14159 * t)
        return np.array([0.1 + 11.5 * t, 0.18 * math.sin(5.65 * t) + arch + 0.9 * t ** 3, 0.0])
    end = spine_at(1.0)
    last = end - spine_at(31 / 32)
    tail = end + last / np.linalg.norm(last) * 1.7
    head = np.array([-1.33, -0.12, 0.0])       # the snout capsule's end, less its radius
    return {"head": head, "tail": tail}
