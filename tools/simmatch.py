#!/usr/bin/env python3
"""Prototype open small-area matcher, and a way to measure whether it works.

Small-area sensors cannot be matched on minutiae (a 4.5 mm square patch has
1-2 of them). The alternative, which is what small-area matchers do, is to
compare the ridge pattern itself: band-pass the image to the ridge frequency,
then score the best normalised cross-correlation over a search in translation
and rotation.

Nothing here is derived from any vendor binary.
"""
import sys, pathlib, itertools
import numpy as np

W = H = 96

def load(p):
    d = p.read_bytes()
    return np.frombuffer(d[d.index(b'255\n')+4:], dtype=np.uint8).reshape(H, W).astype(float)

def ridge_filter(a, period=10.7, width=0.6):
    """Keep the ridge band; kills illumination gradients and fine noise."""
    f = np.fft.fftshift(np.fft.fft2(a - a.mean()))
    ys, xs = np.mgrid[0:H, 0:W]
    r = np.hypot(ys - H/2, xs - W/2)
    r0 = W / period
    f *= np.exp(-((r - r0) ** 2) / (2 * (width * r0) ** 2))
    out = np.real(np.fft.ifft2(np.fft.ifftshift(f)))
    s = out.std()
    return out / s if s > 1e-9 else out

def finger_mask(a, frac=0.35):
    """Rough finger-present mask: local contrast above a fraction of the mean."""
    k = 8
    pad = np.pad(a, k, mode='edge')
    loc = np.zeros_like(a)
    for dy in range(-k, k+1, 4):
        for dx in range(-k, k+1, 4):
            loc += np.abs(pad[k+dy:k+dy+H, k+dx:k+dx+W] - a)
    return loc > frac * loc.mean()

def rotate(a, deg):
    if deg == 0:
        return a
    t = np.deg2rad(deg)
    cy = cx = (W - 1) / 2
    ys, xs = np.mgrid[0:H, 0:W]
    sy = (ys - cy) * np.cos(t) + (xs - cx) * np.sin(t) + cy
    sx = -(ys - cy) * np.sin(t) + (xs - cx) * np.cos(t) + cx
    y0 = np.floor(sy).astype(int); x0 = np.floor(sx).astype(int)
    fy = sy - y0; fx = sx - x0
    ok = (y0 >= 0) & (y0 < H-1) & (x0 >= 0) & (x0 < W-1)
    y0c = np.clip(y0, 0, H-2); x0c = np.clip(x0, 0, W-2)
    out = ((1-fy)*((1-fx)*a[y0c, x0c] + fx*a[y0c, x0c+1]) +
               fy *((1-fx)*a[y0c+1, x0c] + fx*a[y0c+1, x0c+1]))
    return np.where(ok, out, 0.0)

def score(fa, ma, b, max_shift=24, angles=range(-12, 13, 3)):
    """Best masked NCC of b against fa over rotation and translation."""
    best = -1.0
    Fa = np.fft.fft2(fa * ma)
    Ma = np.fft.fft2(ma.astype(float))
    for deg in angles:
        fb = ridge_filter(rotate(b, deg))
        mb = finger_mask(rotate(b, deg))
        fbm = fb * mb
        # cross-correlation and overlap count, via FFT
        num = np.real(np.fft.ifft2(Fa * np.conj(np.fft.fft2(fbm))))
        cnt = np.real(np.fft.ifft2(Ma * np.conj(np.fft.fft2(mb.astype(float)))))
        ea = np.real(np.fft.ifft2(np.fft.fft2(fa**2 * ma) * np.conj(np.fft.fft2(mb.astype(float)))))
        eb = np.real(np.fft.ifft2(Ma * np.conj(np.fft.fft2(fb**2 * mb))))
        with np.errstate(invalid='ignore', divide='ignore'):
            ncc = num / np.sqrt(np.maximum(ea, 1e-9) * np.maximum(eb, 1e-9))
        # only consider small shifts and a decent overlap
        sh = np.zeros_like(ncc, bool)
        sh[:max_shift, :max_shift] = True; sh[:max_shift, -max_shift:] = True
        sh[-max_shift:, :max_shift] = True; sh[-max_shift:, -max_shift:] = True
        valid = sh & (cnt > 0.25 * ma.sum())
        if valid.any():
            best = max(best, float(np.nanmax(ncc[valid])))
    return best

if __name__ == "__main__":
    S = pathlib.Path(sys.argv[1])
    files = sorted(S.glob("img/frame*.pgm"))
    imgs = [load(f) for f in files]
    prep = [(ridge_filter(a), finger_mask(a)) for a in imgs]
    n = len(imgs)
    print(f"similarity matrix over {n} frames of one Windows enrolment session")
    M = np.zeros((n, n))
    for i in range(n):
        for j in range(n):
            M[i, j] = 1.0 if i == j else score(prep[i][0], prep[i][1], imgs[j])
    np.save(S / "simmatrix.npy", M)
    print("     " + " ".join(f"{j+1:>4}" for j in range(n)))
    for i in range(n):
        print(f" {i+1:>3} " + " ".join(f"{M[i,j]:4.2f}" for j in range(n)))
    off = M[~np.eye(n, dtype=bool)]
    print(f"\n  off-diagonal: min {off.min():.2f}  median {np.median(off):.2f}  "
          f"max {off.max():.2f}  mean {off.mean():.2f}")
