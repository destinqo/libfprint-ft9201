/*
 * Measures the effect of a turn on the matcher of the driver.
 *
 * The driver says that it is not sensitive to a turn: the descriptor turns
 * its sample grid by the orientation of the keypoint, and the RANSAC model
 * estimates a turn of any angle. This program tests that statement on real
 * frames instead of reading the code.
 *
 * It turns each frame by an angle, and it scores the turned frame against
 * the frame that is not turned. The baseline is a turn of 0 degrees, which
 * uses the same bilinear resample. Thus the result shows the effect of the
 * turn alone, and not the effect of the resample.
 *
 * Build:
 *   ./extract-matcher.sh
 *   gcc -O2 -o rotation-test rotation-test.c $(pkg-config --cflags --libs glib-2.0) -lm
 * Use:
 *   ./rotation-test  frames/frame01.pgm ...
 *
 * Copyright (C) 2026 Miroslav Baranko <miroslav.baranko@upjs.sk>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include <glib.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "ft9201-matcher-impl.inc"

#define MAX_DIM 128
#define MAX_FR  512

/* Turns the image by the angle, about its centre. A pixel outside the
 * source takes the mean value, thus the border makes no false edge. */
static void
rotate (const guint8 *src, guint8 *dst, gint w, gint h, gfloat deg)
{
  gfloat rad = deg * (gfloat) G_PI / 180.0f;
  gfloat ca = cosf (rad), sa = sinf (rad);
  gfloat cx = (w - 1) / 2.0f, cy = (h - 1) / 2.0f;
  gdouble sum = 0;
  guint8 fill;

  for (gint i = 0; i < w * h; i++)
    sum += src[i];
  fill = (guint8) (sum / (w * h));

  for (gint y = 0; y < h; y++)
    for (gint x = 0; x < w; x++)
      {
        gfloat dx = x - cx, dy = y - cy;
        gfloat sx = dx * ca + dy * sa + cx;
        gfloat sy = -dx * sa + dy * ca + cy;
        gint x0 = (gint) floorf (sx), y0 = (gint) floorf (sy);
        gfloat fx = sx - x0, fy = sy - y0;
        gfloat v;

        if (x0 < 0 || y0 < 0 || x0 + 1 >= w || y0 + 1 >= h)
          {
            dst[y * w + x] = fill;
            continue;
          }
        v = src[y0 * w + x0] * (1 - fx) * (1 - fy)
            + src[y0 * w + x0 + 1] * fx * (1 - fy)
            + src[(y0 + 1) * w + x0] * (1 - fx) * fy
            + src[(y0 + 1) * w + x0 + 1] * fx * fy;
        dst[y * w + x] = (guint8) CLAMP ((gint) (v + 0.5f), 0, 255);
      }
}

static gboolean
load_pgm (const char *path, guint8 *px, gint *w, gint *h)
{
  FILE *f = fopen (path, "rb");
  char magic[3] = { 0 };
  int ww, hh, maxv;

  if (f == NULL)
    return FALSE;
  if (fscanf (f, "%2s %d %d %d", magic, &ww, &hh, &maxv) != 4 ||
      strcmp (magic, "P5") != 0 || ww > MAX_DIM || hh > MAX_DIM)
    {
      fclose (f);
      return FALSE;
    }
  fgetc (f);
  if (fread (px, 1, (gsize) ww * hh, f) != (gsize) ww * hh)
    {
      fclose (f);
      return FALSE;
    }
  *w = ww;
  *h = hh;
  fclose (f);
  return TRUE;
}

int
main (int argc, char **argv)
{
  static guint8 src[MAX_DIM * MAX_DIM], rot[MAX_DIM * MAX_DIM];
  static const gfloat angles[] = { 0, 5, 10, 15, 20, 30, 45, 60, 90 };
  gint n = 0;
  gdouble sum[G_N_ELEMENTS (angles)] = { 0 };
  gint cnt[G_N_ELEMENTS (angles)] = { 0 };

  if (argc < 2)
    {
      fprintf (stderr, "usage: %s frames/*.pgm\n", argv[0]);
      return 2;
    }

  for (gint a = 1; a < argc && n < MAX_FR; a++)
    {
      gint w, h;
      g_autoptr (Ft9201Features) fs = NULL;

      if (!load_pgm (argv[a], src, &w, &h))
        continue;
      fs = ft9201_features_new (src, w, h);
      if (fs == NULL)
        continue;
      n++;

      for (guint i = 0; i < G_N_ELEMENTS (angles); i++)
        {
          g_autoptr (Ft9201Features) fr = NULL;

          rotate (src, rot, w, h, angles[i]);
          fr = ft9201_features_new (rot, w, h);
          if (fr == NULL)
            continue;
          sum[i] += ft9201_match_pair (fs, fr);
          cnt[i]++;
        }
    }

  printf ("%d frames\n\n", n);
  printf ("  turn    mean score   relative to 0 degrees\n");
  for (guint i = 0; i < G_N_ELEMENTS (angles); i++)
    {
      gdouble m = cnt[i] ? sum[i] / cnt[i] : 0;
      gdouble base = cnt[0] ? sum[0] / cnt[0] : 1;

      printf ("  %3.0f deg   %8.3f     %6.1f %%\n",
              (gdouble) angles[i], m, base > 0 ? 100.0 * m / base : 0.0);
    }
  return 0;
}
