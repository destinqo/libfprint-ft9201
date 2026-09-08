#!/usr/bin/env python3
"""Test whether stitching several frames of one finger yields a bigger
image, the way libfprint's swipe drivers do. Measures how far apart
consecutive presses actually land."""
import sys, pathlib, itertools
import numpy as np

def load(p):
    d = p.read_bytes()
    return np.frombuffer(d[d.index(b'255\n')+4:], dtype=np.uint8).reshape(96,96).astype(float)

def phase_corr(a, b):
    """Return (dy, dx, peak) aligning b onto a."""
    A = np.fft.fft2(a - a.mean()); B = np.fft.fft2(b - b.mean())
    R = A * np.conj(B)
    R /= np.maximum(np.abs(R), 1e-9)
    c = np.real(np.fft.ifft2(R))
    idx = np.unravel_index(np.argmax(c), c.shape)
    dy, dx = idx
    if dy > 48: dy -= 96
    if dx > 48: dx -= 96
    # normalised cross-correlation at that shift, as a quality measure
    bs = np.roll(np.roll(b, dy, 0), dx, 1)
    ys = slice(max(dy,0), 96+min(dy,0)); xs = slice(max(dx,0), 96+min(dx,0))
    p, q = a[ys,xs].ravel(), bs[ys,xs].ravel()
    if p.size < 100: return dy, dx, 0.0
    p = p - p.mean(); q = q - q.mean()
    denom = np.sqrt((p*p).sum() * (q*q).sum())
    return dy, dx, float((p*q).sum()/denom) if denom else 0.0

src = pathlib.Path(sys.argv[1])
frames = sorted(src.glob("img/frame*.pgm"))
imgs = [load(f) for f in frames]
print(f"{len(imgs)} frames from one Windows enrolment session\n")

ref = 0
print("  frame   dy   dx   corr   (offset of each frame vs frame01)")
offs = [(0,0,1.0)]
for i in range(1, len(imgs)):
    dy, dx, c = phase_corr(imgs[ref], imgs[i])
    offs.append((dy,dx,c))
    print(f"   {i+1:>3}  {dy:>4} {dx:>4}   {c:5.2f}")

good = [(dy,dx) for dy,dx,c in offs if c > 0.30]
print(f"\n  frames aligning with corr > 0.30: {len(good)} of {len(imgs)}")
if good:
    ys = [d[0] for d in good]; xs = [d[1] for d in good]
    span_y = max(ys)-min(ys)+96; span_x = max(xs)-min(xs)+96
    print(f"  mosaic bounding box would be {span_x} x {span_y} px "
          f"(vs 96 x 96 for one frame)")
    print(f"  area gain: {span_x*span_y/(96*96):.2f}x")
