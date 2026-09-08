#!/usr/bin/env python3
"""Produce enhanced variants of captured frames, to test whether any
preprocessing raises the minutiae count NBIS can find."""
import sys, pathlib
import numpy as np

def load(p):
    d = p.read_bytes()
    return np.frombuffer(d[d.index(b'255\n')+4:], dtype=np.uint8).reshape(96, 96).astype(float)

def save(p, a):
    a = np.clip(a, 0, 255).astype(np.uint8)
    p.write_bytes(b"P5\n96 96\n255\n" + a.tobytes())

def norm(a):
    lo, hi = a.min(), a.max()
    return (a - lo) * (255.0 / (hi - lo)) if hi > lo else a * 0

def hist_eq(a):
    h, _ = np.histogram(a.astype(np.uint8), bins=256, range=(0, 256))
    cdf = np.cumsum(h).astype(float)
    cdf = (cdf - cdf.min()) * 255.0 / (cdf.max() - cdf.min())
    return cdf[a.astype(np.uint8)]

def clahe(a, tiles=6, clip=3.0):
    """Tiled histogram equalisation with a clip limit, bilinearly blended."""
    out = np.zeros_like(a)
    ts = 96 // tiles
    maps = np.zeros((tiles, tiles, 256))
    for ty in range(tiles):
        for tx in range(tiles):
            blk = a[ty*ts:(ty+1)*ts, tx*ts:(tx+1)*ts].astype(np.uint8)
            h, _ = np.histogram(blk, bins=256, range=(0, 256))
            limit = clip * blk.size / 256.0
            excess = np.maximum(h - limit, 0).sum()
            h = np.minimum(h, limit) + excess / 256.0
            cdf = np.cumsum(h)
            maps[ty, tx] = (cdf - cdf.min()) * 255.0 / max(cdf.max() - cdf.min(), 1e-9)
    for y in range(96):
        fy = min(max(y / ts - 0.5, 0), tiles - 1)
        y0 = int(fy); y1 = min(y0 + 1, tiles - 1); wy = fy - y0
        for x in range(96):
            fx = min(max(x / ts - 0.5, 0), tiles - 1)
            x0 = int(fx); x1 = min(x0 + 1, tiles - 1); wx = fx - x0
            v = a[y, x].astype(np.uint8)
            out[y, x] = ((1-wy)*((1-wx)*maps[y0,x0,v] + wx*maps[y0,x1,v]) +
                             wy *((1-wx)*maps[y1,x0,v] + wx*maps[y1,x1,v]))
    return out

def bandpass(a, period=10.7, width=0.6):
    """Keep only the ridge spatial frequency band."""
    f = np.fft.fftshift(np.fft.fft2(a - a.mean()))
    ys, xs = np.mgrid[0:96, 0:96]
    r = np.sqrt((ys-48)**2 + (xs-48)**2)
    r0 = 96.0 / period
    mask = np.exp(-((r - r0)**2) / (2 * (width * r0)**2))
    return norm(np.real(np.fft.ifft2(np.fft.ifftshift(f * mask))))

def unsharp(a, amount=1.5):
    k = np.array([[1,2,1],[2,4,2],[1,2,1]], float); k /= k.sum()
    pad = np.pad(a, 1, mode='edge')
    blur = sum(k[i, j] * pad[i:i+96, j:j+96] for i in range(3) for j in range(3))
    return norm(a + amount * (a - blur))

VARIANTS = {
    "raw":        lambda a: a,
    "histeq":     hist_eq,
    "clahe":      clahe,
    "bandpass":   bandpass,
    "unsharp":    unsharp,
    "clahe+band": lambda a: bandpass(clahe(a)),
    "histeq+band": lambda a: bandpass(hist_eq(a)),
}

src = pathlib.Path(sys.argv[1])
outroot = pathlib.Path(sys.argv[2])
frames = sorted(list(src.glob("img/frame*.pgm")) + list(src.glob("linux-frame0*.pgm")))
print(f"{len(frames)} frames")
for name, fn in VARIANTS.items():
    d = outroot / name
    d.mkdir(parents=True, exist_ok=True)
    for f in frames:
        save(d / f.name, fn(load(f)))
    print(f"  wrote {name}")
