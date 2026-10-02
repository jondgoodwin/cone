"""Texture statistics per region, on the lightness channel L*.

For each region, square windows wholly (90%) inside it are taken on a grid,
and over them:
- spectral slope beta: the radially averaged power spectrum (Hann window,
  mean removed) falls as f^-beta between 1/32 and 1/3 cycles per pixel; for a
  fractional-Brownian surface beta = 2H + 2, so H = (beta - 2) / 2 (clipped
  to 0..1 when reported as H). A steeper slope is smoother.
- dominant orientation and coherence: the structure tensor (Sobel gradients,
  Gaussian-pooled); the grain runs perpendicular to the dominant gradient.
  Coherence (l1 - l2) / (l1 + l2): 0 isotropic, 1 one direction only.
  Reported in the image and relative to the spine's direction there.
- feature scale: over a Gabor bank (wavelengths 4 to 32 px, six
  orientations), the wavelength whose mean energy stands highest above a
  power-law fit of energy against wavelength (the "spectral hump"), with
  that excess as its strength; and the spectrum's centroid wavelength.
- edge density: the share of Canny edge pixels (thresholds 20/60 on L*
  scaled to 0..255).

Which dial each maps to (the `noise` and `sculpt` packages):
- H -> fbm/ridged/billow `gain` = lacunarity^-H (gain 0.5 at H = 1 with
  lacunarity 2); cascade2's H. The rock-type vector's H (fractal-variation.md
  section 3.4, 8.1 rows 5-6).
- orientation, coherence -> phacelle2's `dir`; the rib phase field's
  direction (RibField); coherence high = ribs/grain, low = isotropic fBm or
  more `warp` strength. The rock vector's anisotropy (ratio, angle).
- feature scale -> the noise's base frequency (1 / wavelength, in the
  model's units once the pixel scale is known), phacelle2's `freq`, the rib
  field's `f`; the rock vector's spectral hump.
- edge density -> octaves and amplitude of the detail (how many octaves
  reach the pixel footprint), and the rib/plating cover threshold.
"""

import math

import cv2
import numpy as np

WIN = 64


def _radial_spectrum(win):
    w = win - win.mean()
    hann = np.outer(np.hanning(WIN), np.hanning(WIN))
    F = np.fft.fftshift(np.fft.fft2(w * hann))
    P = np.abs(F) ** 2
    yy, xx = np.indices(P.shape)
    r = np.hypot(yy - WIN // 2, xx - WIN // 2).astype(int)
    rad = np.bincount(r.ravel(), P.ravel()) / np.maximum(np.bincount(r.ravel()), 1)
    f = np.arange(len(rad)) / WIN
    return f, rad


def _gabor_bank(lams=(4, 6, 8, 12, 16, 24, 32), n_theta=6):
    bank = []
    for lam in lams:
        sigma = 0.56 * lam
        ks = int(2 * math.ceil(2.5 * sigma) + 1)
        ks = min(ks, 2 * WIN - 1)
        for k in range(n_theta):
            th = math.pi * k / n_theta
            kr = cv2.getGaborKernel((ks, ks), sigma, th, lam, 0.5, 0, ktype=cv2.CV_32F)
            ki = cv2.getGaborKernel((ks, ks), sigma, th, lam, 0.5, math.pi / 2, ktype=cv2.CV_32F)
            kr -= kr.mean()
            norm = np.sqrt((kr ** 2).sum()) + 1e-9
            bank.append((lam, th, kr / norm, ki / norm))
    return bank


_BANK = None


def windows(region_mask, step=WIN // 2, fill=0.9):
    h, w = region_mask.shape
    ii = cv2.integral(region_mask.astype(np.uint8))
    out = []
    for y in range(0, h - WIN, step):
        for x in range(0, w - WIN, step):
            s = ii[y + WIN, x + WIN] - ii[y, x + WIN] - ii[y + WIN, x] + ii[y, x]
            if s >= fill * WIN * WIN:
                out.append((x, y))
    return out


def structure_tensor(L, sigma=4.0):
    gx = cv2.Sobel(L, cv2.CV_32F, 1, 0, ksize=3)
    gy = cv2.Sobel(L, cv2.CV_32F, 0, 1, ksize=3)
    jxx = cv2.GaussianBlur(gx * gx, (0, 0), sigma)
    jyy = cv2.GaussianBlur(gy * gy, (0, 0), sigma)
    jxy = cv2.GaussianBlur(gx * gy, (0, 0), sigma)
    return jxx, jyy, jxy


def orientation_field(L, sigma=4.0):
    jxx, jyy, jxy = structure_tensor(L, sigma)
    tr = jxx + jyy
    diff = np.sqrt((jxx - jyy) ** 2 + 4 * jxy ** 2)
    coh = np.where(tr > 1e-6, diff / (tr + 1e-9), 0)
    grad_ang = 0.5 * np.arctan2(2 * jxy, jxx - jyy)        # dominant gradient
    grain = grad_ang + np.pi / 2                            # the grain runs across it
    return grain, coh, tr


def region_stats(L, region_mask, spine_dir_deg=None, max_windows=60, seed=3):
    global _BANK
    if _BANK is None:
        _BANK = _gabor_bank()
    wins = windows(region_mask)
    if not wins:
        return None
    rng = np.random.default_rng(seed)
    if len(wins) > max_windows:
        wins = [wins[i] for i in sorted(rng.choice(len(wins), max_windows, replace=False))]
    Lf = L.astype(np.float32)
    betas, cents = [], []
    gab = {}
    for x, y in wins:
        win = Lf[y:y + WIN, x:x + WIN].astype(np.float64)
        f, P = _radial_spectrum(win)
        sel = (f >= 1 / 32) & (f <= 1 / 3) & (P > 0)
        if sel.sum() > 4:
            beta = -np.polyfit(np.log(f[sel]), np.log(P[sel]), 1)[0]
            betas.append(beta)
            band = (f > 0) & (f <= 0.5)
            cents.append(float((f[band] * P[band]).sum() / (P[band].sum() + 1e-12)))
        for lam, th, kr, ki in _BANK:
            r = cv2.filter2D(Lf[y:y + WIN, x:x + WIN], -1, kr)
            i = cv2.filter2D(Lf[y:y + WIN, x:x + WIN], -1, ki)
            gab.setdefault(lam, []).append(float((r * r + i * i).mean()))
    # the orientation over the whole region (energy-weighted doubled angles)
    grain, coh, tr = orientation_field(Lf)
    sel = region_mask > 0
    wgt = (tr * coh)[sel]
    c2 = (wgt * np.cos(2 * grain[sel])).sum()
    s2 = (wgt * np.sin(2 * grain[sel])).sum()
    dom = 0.5 * math.degrees(math.atan2(s2, c2))
    mean_coh = float(np.hypot(c2, s2) / (np.abs(wgt).sum() + 1e-9))   # how consistent across the region
    local_coh = float(np.average(coh[sel], weights=tr[sel] + 1e-9))
    # Gabor: scale-normalised energy (multiply by wavelength so white noise
    # does not pick the finest filter)
    lam_energy = {lam: float(np.mean(v)) for lam, v in gab.items()}
    # A fractal surface's energy rises smoothly with wavelength, so the
    # feature scale is the hump above that power law: the wavelength whose
    # energy most exceeds a straight-line fit in log-log
    ll = np.log(np.array(sorted(lam_energy)))
    le = np.log(np.array([lam_energy[k] for k in sorted(lam_energy)]) + 1e-12)
    resid = le - np.polyval(np.polyfit(ll, le, 1), ll)
    lam_best = sorted(lam_energy)[int(np.argmax(resid))]
    hump = float(resid.max())
    L8 = np.clip(Lf * 2.55, 0, 255).astype(np.uint8)
    edges = cv2.Canny(L8, 20, 60)
    beta = float(np.median(betas)) if betas else float("nan")
    rel = None
    if spine_dir_deg is not None:
        rel = ((dom - spine_dir_deg + 90) % 180) - 90
    return {
        "windows": len(wins),
        "spectral_slope_beta": beta,
        "hurst_H": float(np.clip((beta - 2) / 2, 0, 1)) if betas else None,
        "hurst_H_unclipped": (beta - 2) / 2 if betas else None,
        "grain_deg_image": dom,
        "grain_deg_to_spine": rel,
        "coherence_local": local_coh,
        "coherence_region": mean_coh,
        "gabor_wavelength_px": lam_best,
        "gabor_hump_strength": hump,   # log-energy above the power law; near 0: no preferred scale
        "gabor_energy_by_wavelength": lam_energy,
        "spectral_centroid_wavelength_px": float(1 / np.median(cents)) if cents else None,
        "edge_density": float((edges[sel] > 0).mean()),
        "L_mean": float(Lf[sel].mean()),
        "L_std": float(Lf[sel].std()),
    }


def draw_orientation(bgr, L, mask, step=24, sigma=4.0):
    grain, coh, tr = orientation_field(L.astype(np.float32), sigma)
    out = (bgr.astype(np.float32) * 0.45).astype(np.uint8)
    h, w = mask.shape
    from common import put_label
    for y in range(step // 2, h, step):
        for x in range(step // 2, w, step):
            if not mask[y, x]:
                continue
            c = float(coh[y, x])
            a = float(grain[y, x])
            ln = 0.5 * step * (0.25 + 0.75 * c)
            dx, dy = ln * math.cos(a), ln * math.sin(a)
            col = (int(255 * (1 - c)), int(80 + 175 * c), int(255 * c))
            cv2.line(out, (int(x - dx), int(y - dy)), (int(x + dx), int(y + dy)), col, 2, cv2.LINE_AA)
    put_label(out, "grain direction (structure tensor); long + yellow = coherent, short + blue = isotropic", (12, 26), 0.6)
    return out
