/* Open small-area fingerprint matcher for the FT9201, written to be lifted
 * into the libfprint driver as-is.
 *
 * Why not minutiae: a 96x96 frame from this sensor yields 1-2 minutiae and
 * NBIS/bozorth3 refuses to score fewer than 10, so the minutiae route cannot
 * work here at all. What does work on a small area is comparing the ridge
 * pattern itself: band-pass the frame to the ridge frequency, mask off the
 * part of the sensor the finger is not touching, and take the best
 * normalised cross-correlation over a search in translation and rotation.
 *
 * Nothing here is derived from any vendor binary; it is the standard
 * correlation approach for small-area sensors.
 *
 * Standalone driver for evaluation:
 *   gcc -O2 -o ft9201-matcher ft9201-matcher.c -lm
 *   ./ft9201-matcher <dir-of-pgms> [more dirs...]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* ---------- the matcher: plain C, no dependencies beyond libm ---------- */

#define FT9201_MATCH_MAX_DIM   128
#define FT9201_MATCH_MAX_PX    (FT9201_MATCH_MAX_DIM * FT9201_MATCH_MAX_DIM)

/* Ridge wavelength in pixels, measured on this sensor: 9.6-13.7 across 23
 * frames, median 10.7. The band-pass is centred here. */
#define FT9201_RIDGE_PERIOD    10.7f

/* Search bounds. Presses land within a few pixels of each other on a sensor
 * this small, so a wide search only adds false-accept risk and cost. */
#define FT9201_MATCH_MAX_SHIFT 12
#define FT9201_MATCH_SHIFT_STEP 2
#define FT9201_MATCH_MAX_ANGLE 9
#define FT9201_MATCH_ANGLE_STEP 3

/* A pose is only scored if the two finger masks overlap by at least this
 * fraction of the smaller mask -- otherwise a tiny well-correlated corner
 * could score high. */
#define FT9201_MATCH_MIN_OVERLAP 0.35f

typedef struct
{
  int   w, h;
  float filtered[FT9201_MATCH_MAX_PX];
  unsigned char mask[FT9201_MATCH_MAX_PX];
  int   mask_count;
} Ft9201Features;

/* Separable Gaussian blur with edge clamping. */
static void
ft9201_blur (const float *src, float *dst, int w, int h, float sigma)
{
#define FT9201_BLUR_MAX_R 24
  int r = (int) (3.0f * sigma + 0.5f);
  if (r < 1) { memcpy (dst, src, sizeof (float) * w * h); return; }
  if (r > FT9201_BLUR_MAX_R) r = FT9201_BLUR_MAX_R;
  float k[2 * FT9201_BLUR_MAX_R + 1];
  float sum = 0.0f;
  for (int i = -r; i <= r; i++)
    { k[i + r] = expf (-(i * i) / (2.0f * sigma * sigma)); sum += k[i + r]; }
  for (int i = 0; i <= 2 * r; i++) k[i] /= sum;

  static float tmp[FT9201_MATCH_MAX_PX];
  for (int y = 0; y < h; y++)
    for (int x = 0; x < w; x++)
      {
        float a = 0.0f;
        for (int i = -r; i <= r; i++)
          {
            int xx = x + i;
            if (xx < 0) xx = 0; else if (xx >= w) xx = w - 1;
            a += k[i + r] * src[y * w + xx];
          }
        tmp[y * w + x] = a;
      }
  for (int y = 0; y < h; y++)
    for (int x = 0; x < w; x++)
      {
        float a = 0.0f;
        for (int i = -r; i <= r; i++)
          {
            int yy = y + i;
            if (yy < 0) yy = 0; else if (yy >= h) yy = h - 1;
            /* second pass reads the first pass, not the input */
            a += k[i + r] * tmp[yy * w + x];
          }
        dst[y * w + x] = a;
      }
}

/* Band-pass around the ridge frequency, as a difference of Gaussians. This
 * removes the illumination gradient and the fine noise, leaving the ridges. */
static void
ft9201_ridge_filter (const float *in, float *out, int w, int h)
{
  static float lo[FT9201_MATCH_MAX_PX], hi[FT9201_MATCH_MAX_PX];
  ft9201_blur (in, hi, w, h, FT9201_RIDGE_PERIOD / 6.0f);
  ft9201_blur (in, lo, w, h, FT9201_RIDGE_PERIOD / 2.0f);
  for (int i = 0; i < w * h; i++)
    out[i] = hi[i] - lo[i];
}

/* Where is the finger? Ridge energy, smoothed, above a fraction of its mean. */
static void
ft9201_finger_mask (const float *f, unsigned char *mask, int *count,
                    int w, int h)
{
  static float energy[FT9201_MATCH_MAX_PX], sm[FT9201_MATCH_MAX_PX];
  for (int i = 0; i < w * h; i++) energy[i] = f[i] * f[i];
  ft9201_blur (energy, sm, w, h, FT9201_RIDGE_PERIOD / 2.0f);
  double mean = 0.0;
  for (int i = 0; i < w * h; i++) mean += sm[i];
  mean /= (w * h);
  int n = 0;
  for (int i = 0; i < w * h; i++)
    { mask[i] = sm[i] > 0.30 * mean; n += mask[i]; }
  *count = n;
}

static void
ft9201_features (const unsigned char *img, int w, int h, Ft9201Features *out)
{
  static float f[FT9201_MATCH_MAX_PX];
  out->w = w; out->h = h;
  for (int i = 0; i < w * h; i++) f[i] = (float) img[i];
  ft9201_ridge_filter (f, out->filtered, w, h);
  ft9201_finger_mask (out->filtered, out->mask, &out->mask_count, w, h);
}

/* Rotate an 8-bit image about its centre, bilinear, zero outside. */
static void
ft9201_rotate (const unsigned char *src, unsigned char *dst,
               int w, int h, float deg)
{
  if (deg == 0.0f) { memcpy (dst, src, (size_t) w * h); return; }
  float t = deg * (float) M_PI / 180.0f;
  float c = cosf (t), s = sinf (t);
  float cy = (h - 1) / 2.0f, cx = (w - 1) / 2.0f;
  for (int y = 0; y < h; y++)
    for (int x = 0; x < w; x++)
      {
        float dy = y - cy, dx = x - cx;
        float sy = dy * c + dx * s + cy;
        float sx = -dy * s + dx * c + cx;
        int y0 = (int) floorf (sy), x0 = (int) floorf (sx);
        if (y0 < 0 || x0 < 0 || y0 >= h - 1 || x0 >= w - 1)
          { dst[y * w + x] = 0; continue; }
        float fy = sy - y0, fx = sx - x0;
        float v = (1 - fy) * ((1 - fx) * src[y0 * w + x0] + fx * src[y0 * w + x0 + 1])
                  +    fy  * ((1 - fx) * src[(y0 + 1) * w + x0] + fx * src[(y0 + 1) * w + x0 + 1]);
        dst[y * w + x] = (unsigned char) (v + 0.5f);
      }
}

/* Masked normalised cross-correlation of b shifted by (dy,dx) onto a. */
static float
ft9201_ncc (const Ft9201Features *a, const Ft9201Features *b, int dy, int dx)
{
  int w = a->w, h = a->h;
  double sa = 0, sb = 0, saa = 0, sbb = 0, sab = 0;
  int n = 0;
  for (int y = 0; y < h; y++)
    {
      int by = y + dy;
      if (by < 0 || by >= h) continue;
      for (int x = 0; x < w; x++)
        {
          int bx = x + dx;
          if (bx < 0 || bx >= w) continue;
          int ia = y * w + x, ib = by * w + bx;
          if (!a->mask[ia] || !b->mask[ib]) continue;
          float va = a->filtered[ia], vb = b->filtered[ib];
          sa += va; sb += vb; saa += (double) va * va;
          sbb += (double) vb * vb; sab += (double) va * vb;
          n++;
        }
    }
  int smaller = a->mask_count < b->mask_count ? a->mask_count : b->mask_count;
  if (n < 200 || n < FT9201_MATCH_MIN_OVERLAP * smaller)
    return -1.0f;
  double na = saa - sa * sa / n, nb = sbb - sb * sb / n;
  if (na <= 1e-9 || nb <= 1e-9) return -1.0f;
  return (float) ((sab - sa * sb / n) / sqrt (na * nb));
}

/* Best score of probe against gallery over rotation and translation. */
static float
ft9201_match_score (const unsigned char *gallery, const unsigned char *probe,
                    int w, int h)
{
  static Ft9201Features fg, fp;
  static unsigned char rot[FT9201_MATCH_MAX_PX];
  ft9201_features (gallery, w, h, &fg);

  float best = -1.0f;
  int best_dy = 0, best_dx = 0; float best_deg = 0.0f;

  for (int deg = -FT9201_MATCH_MAX_ANGLE; deg <= FT9201_MATCH_MAX_ANGLE;
       deg += FT9201_MATCH_ANGLE_STEP)
    {
      ft9201_rotate (probe, rot, w, h, (float) deg);
      ft9201_features (rot, w, h, &fp);
      for (int dy = -FT9201_MATCH_MAX_SHIFT; dy <= FT9201_MATCH_MAX_SHIFT;
           dy += FT9201_MATCH_SHIFT_STEP)
        for (int dx = -FT9201_MATCH_MAX_SHIFT; dx <= FT9201_MATCH_MAX_SHIFT;
             dx += FT9201_MATCH_SHIFT_STEP)
          {
            float s = ft9201_ncc (&fg, &fp, dy, dx);
            if (s > best) { best = s; best_dy = dy; best_dx = dx; best_deg = deg; }
          }
    }

  /* Refine around the best pose. */
  for (float deg = best_deg - 2.0f; deg <= best_deg + 2.0f; deg += 1.0f)
    {
      ft9201_rotate (probe, rot, w, h, deg);
      ft9201_features (rot, w, h, &fp);
      for (int dy = best_dy - 2; dy <= best_dy + 2; dy++)
        for (int dx = best_dx - 2; dx <= best_dx + 2; dx++)
          {
            float s = ft9201_ncc (&fg, &fp, dy, dx);
            if (s > best) best = s;
          }
    }
  return best;
}

/* ---------- evaluation harness (not part of the driver) ---------- */

typedef struct { char name[256]; int label; unsigned char px[FT9201_MATCH_MAX_PX]; int w, h; } Frame;

static int
load_pgm (const char *path, Frame *f)
{
  FILE *fh = fopen (path, "rb");
  if (!fh) return 0;
  char magic[3] = {0};
  int w, h, maxv;
  if (fscanf (fh, "%2s %d %d %d", magic, &w, &h, &maxv) != 4 ||
      strcmp (magic, "P5") || w > FT9201_MATCH_MAX_DIM || h > FT9201_MATCH_MAX_DIM)
    { fclose (fh); return 0; }
  fgetc (fh);
  if (fread (f->px, 1, (size_t) w * h, fh) != (size_t) w * h) { fclose (fh); return 0; }
  f->w = w; f->h = h;
  fclose (fh);
  return 1;
}

int
main (int argc, char **argv)
{
  static Frame frames[128];
  int n = 0;
  for (int a = 1; a < argc && n < 128; a++)
    {
      if (!load_pgm (argv[a], &frames[n])) { fprintf (stderr, "skip %s\n", argv[a]); continue; }
      snprintf (frames[n].name, sizeof frames[n].name, "%s", argv[a]);
      /* label = the directory component, so same-finger sets can be grouped */
      frames[n].label = 0;
      const char *slash = strrchr (argv[a], '/');
      if (slash)
        {
          char dir[256]; size_t len = slash - argv[a];
          if (len >= sizeof dir) len = sizeof dir - 1;
          memcpy (dir, argv[a], len); dir[len] = 0;
          const char *d2 = strrchr (dir, '/');
          const char *tag = d2 ? d2 + 1 : dir;
          for (const char *c = tag; *c; c++) frames[n].label = frames[n].label * 31 + *c;
        }
      n++;
    }
  printf ("%d frames\n\n", n);

  printf ("%-28s", "");
  for (int j = 0; j < n; j++) printf ("%5d", j + 1);
  printf ("\n");
  static float M[128][128];
  for (int i = 0; i < n; i++)
    {
      const char *base = strrchr (frames[i].name, '/');
      printf ("%2d %-25s", i + 1, base ? base + 1 : frames[i].name);
      for (int j = 0; j < n; j++)
        {
          M[i][j] = (i == j) ? 1.0f
                    : ft9201_match_score (frames[i].px, frames[j].px,
                                          frames[i].w, frames[i].h);
          printf ("%5.2f", M[i][j]);
        }
      printf ("\n");
    }

  /* same-label vs different-label statistics */
  double smin = 2, smax = -2, ssum = 0; int sn = 0;
  double dmin = 2, dmax = -2, dsum = 0; int dn = 0;
  for (int i = 0; i < n; i++)
    for (int j = i + 1; j < n; j++)
      {
        float v = (M[i][j] + M[j][i]) / 2.0f;
        if (frames[i].label == frames[j].label)
          { sn++; ssum += v; if (v < smin) smin = v; if (v > smax) smax = v; }
        else
          { dn++; dsum += v; if (v < dmin) dmin = v; if (v > dmax) dmax = v; }
      }
  if (sn) printf ("\nsame group   n=%3d  mean %.2f  min %.2f  max %.2f\n",
                  sn, ssum / sn, smin, smax);
  if (dn) printf ("diff group   n=%3d  mean %.2f  min %.2f  max %.2f\n",
                  dn, dsum / dn, dmin, dmax);
  if (sn && dn)
    {
      printf ("\n threshold   FRR(same rejected)   FAR(diff accepted)\n");
      for (float t = 0.20f; t <= 0.65f; t += 0.05f)
        {
          int fr = 0, fa = 0;
          for (int i = 0; i < n; i++)
            for (int j = i + 1; j < n; j++)
              {
                float v = (M[i][j] + M[j][i]) / 2.0f;
                if (frames[i].label == frames[j].label) fr += (v < t);
                else fa += (v >= t);
              }
          printf ("   %.2f        %5.1f%%              %5.1f%%\n",
                  t, 100.0 * fr / sn, 100.0 * fa / dn);
        }
    }
  return 0;
}
