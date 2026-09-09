/*
 * Tests two changes to the image path: a correction of the fixed pattern
 * of the sensor, and a stretch of the histogram.
 *
 * The project OMGrant/ft9201-libfprint reads a frame of the empty sensor
 * at the start of each scan, and it takes that frame away from the frame
 * of the finger. It then stretches the histogram to the full range. This
 * driver does neither.
 *
 * A frame of the empty sensor is not in the collection, thus the program
 * makes an estimate: the median of each pixel over all frames. The ridges
 * of 13 different fingers are not the same, but the fixed pattern of the
 * sensor is the same in each frame. Thus the median holds the fixed
 * pattern and the mean level of the skin.
 *
 * Build:
 *   ./extract-matcher.sh
 *   gcc -O2 -o fpn-experiment fpn-experiment.c \
 *       $(pkg-config --cflags --libs glib-2.0) -lm
 * Use:
 *   ./fpn-experiment  ../samples/dataset-DATE/finger/frame01.pgm ...
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

typedef struct
{
  char   group[128];
  guint8 px[MAX_DIM * MAX_DIM];
  gint   w, h;
} Frame;

static gboolean
load_pgm (const char *path, Frame *f)
{
  FILE *fh = fopen (path, "rb");
  char magic[3] = { 0 };
  int w, h, maxv;

  if (fh == NULL)
    return FALSE;
  if (fscanf (fh, "%2s %d %d %d", magic, &w, &h, &maxv) != 4 ||
      strcmp (magic, "P5") != 0 || w > MAX_DIM || h > MAX_DIM)
    {
      fclose (fh);
      return FALSE;
    }
  fgetc (fh);
  if (fread (f->px, 1, (gsize) w * h, fh) != (gsize) w * h)
    {
      fclose (fh);
      return FALSE;
    }
  f->w = w;
  f->h = h;
  fclose (fh);
  return TRUE;
}

static void
group_of (const char *path, char *out, gsize outlen)
{
  const char *slash = strrchr (path, '/');
  char dir[256];
  gsize len;

  if (slash == NULL)
    {
      g_strlcpy (out, ".", outlen);
      return;
    }
  len = (gsize) (slash - path);
  if (len >= sizeof dir)
    len = sizeof dir - 1;
  memcpy (dir, path, len);
  dir[len] = '\0';
  slash = strrchr (dir, '/');
  g_strlcpy (out, slash != NULL ? slash + 1 : dir, outlen);
}

static int
cmp_u8 (const void *a, const void *b)
{
  return (int) *(const guint8 *) a - (int) *(const guint8 *) b;
}

/* Takes the fixed pattern away, as OMGrant does: pixel - background + 128,
 * with a limit at 0 and 255. */
static void
apply_fpn (guint8 *dst, const guint8 *src, const guint8 *bg, gint n)
{
  for (gint i = 0; i < n; i++)
    {
      gint v = (gint) src[i] - (gint) bg[i] + 128;

      dst[i] = (guint8) CLAMP (v, 0, 255);
    }
}

/* Stretches the values to the full range. */
static void
apply_stretch (guint8 *dst, const guint8 *src, gint n)
{
  guint8 lo = 255, hi = 0;

  for (gint i = 0; i < n; i++)
    {
      if (src[i] < lo)
        lo = src[i];
      if (src[i] > hi)
        hi = src[i];
    }
  if (hi <= lo)
    {
      memcpy (dst, src, n);
      return;
    }
  for (gint i = 0; i < n; i++)
    dst[i] = (guint8) (((guint) (src[i] - lo) * 255) / (hi - lo));
}

static void
quality (const gfloat *gen, gint gn, const gfloat *imp, gint in,
         gdouble *eer, gdouble *dp)
{
  gdouble mg = 0, mi = 0, vg = 0, vi = 0, best = 2.0;

  for (gint i = 0; i < gn; i++)
    mg += gen[i];
  mg /= gn;
  for (gint i = 0; i < in; i++)
    mi += imp[i];
  mi /= in;
  for (gint i = 0; i < gn; i++)
    vg += (gen[i] - mg) * (gen[i] - mg);
  for (gint i = 0; i < in; i++)
    vi += (imp[i] - mi) * (imp[i] - mi);
  vg /= (gn > 1 ? gn - 1 : 1);
  vi /= (in > 1 ? in - 1 : 1);
  *dp = (vg + vi) > 0 ? (mg - mi) / sqrt ((vg + vi) / 2.0) : 0.0;
  *eer = 1.0;
  for (gdouble t = 0.0; t <= 1.0; t += 0.0005)
    {
      gint fr = 0, fa = 0;
      gdouble frr, far;

      for (gint i = 0; i < gn; i++)
        fr += (gen[i] < t);
      for (gint i = 0; i < in; i++)
        fa += (imp[i] >= t);
      frr = (gdouble) fr / gn;
      far = (gdouble) fa / in;
      if (fabs (frr - far) < best)
        {
          best = fabs (frr - far);
          *eer = (frr + far) / 2.0;
        }
    }
}

/* Scores every frame against a template of the other frames of its group,
 * and against a template of each other group. Returns the two rates. */
static void
evaluate (Frame *fr, gint n, Ft9201Features **feat,
          gfloat *gen, gint *gnp, gfloat *imp, gint *inp)
{
  gint gn = 0, in = 0;

  for (gint i = 0; i < n; i++)
    {
      gfloat first = -1.0f, second = -1.0f;
      gint scored = 0;

      for (gint j = 0; j < n; j++)
        {
          gfloat s;

          if (j == i || strcmp (fr[j].group, fr[i].group) != 0)
            continue;
          s = ft9201_match_pair (feat[j], feat[i]);
          scored++;
          if (s > first)
            {
              second = first;
              first = s;
            }
          else if (s > second)
            {
              second = s;
            }
        }
      if (scored >= 2)
        gen[gn++] = second;

      for (gint j = 0; j < n; j++)
        {
          gboolean seen = FALSE;

          if (strcmp (fr[j].group, fr[i].group) == 0)
            continue;
          for (gint k = 0; k < j; k++)
            if (strcmp (fr[k].group, fr[j].group) == 0)
              seen = TRUE;
          if (seen)
            continue;

          first = second = -1.0f;
          scored = 0;
          for (gint m = 0; m < n; m++)
            {
              gfloat s;

              if (strcmp (fr[m].group, fr[j].group) != 0)
                continue;
              s = ft9201_match_pair (feat[m], feat[i]);
              scored++;
              if (s > first)
                {
                  second = first;
                  first = s;
                }
              else if (s > second)
                {
                  second = s;
                }
            }
          if (scored >= 2)
            imp[in++] = second;
        }
    }
  *gnp = gn;
  *inp = in;
}

static void
run (const char *name, Frame *fr, gint n, const guint8 *bg,
     gboolean fpn, gboolean stretch)
{
  static Ft9201Features *feat[MAX_FR];
  static guint8 buf[MAX_DIM * MAX_DIM], buf2[MAX_DIM * MAX_DIM];
  static gfloat gen[MAX_FR], imp[MAX_FR * 32];
  gint gn = 0, in = 0, npx = fr[0].w * fr[0].h;
  gdouble eer, dp;

  for (gint i = 0; i < n; i++)
    {
      const guint8 *cur = fr[i].px;

      if (fpn)
        {
          apply_fpn (buf, cur, bg, npx);
          cur = buf;
        }
      if (stretch)
        {
          apply_stretch (buf2, cur, npx);
          cur = buf2;
        }
      feat[i] = ft9201_features_new (cur, fr[i].w, fr[i].h);
    }

  evaluate (fr, n, feat, gen, &gn, imp, &in);
  quality (gen, gn, imp, in, &eer, &dp);
  printf ("  %-34s EER %6.2f %%   d' %.2f   (genuine %d, impostor %d)\n",
          name, 100.0 * eer, dp, gn, in);

  for (gint i = 0; i < n; i++)
    ft9201_features_free (feat[i]);
}

int
main (int argc, char **argv)
{
  static Frame fr[MAX_FR];
  static guint8 bg[MAX_DIM * MAX_DIM];
  static guint8 col[MAX_FR];
  gint n = 0;

  for (gint a = 1; a < argc && n < MAX_FR; a++)
    {
      if (!load_pgm (argv[a], &fr[n]))
        continue;
      group_of (argv[a], fr[n].group, sizeof fr[n].group);
      n++;
    }
  if (n < 4)
    {
      fprintf (stderr, "usage: %s frame01.pgm ...\n", argv[0]);
      return 2;
    }

  /* The median of each pixel over all frames is the estimate of the fixed
   * pattern. */
  for (gint p = 0; p < fr[0].w * fr[0].h; p++)
    {
      for (gint i = 0; i < n; i++)
        col[i] = fr[i].px[p];
      qsort (col, n, 1, cmp_u8);
      bg[p] = col[n / 2];
    }

  printf ("%d frames, the background is the median of each pixel\n\n", n);
  run ("the driver, no change", fr, n, bg, FALSE, FALSE);
  run ("stretch of the histogram only", fr, n, bg, FALSE, TRUE);
  run ("fixed pattern only", fr, n, bg, TRUE, FALSE);
  run ("fixed pattern and stretch", fr, n, bg, TRUE, TRUE);
  return 0;
}
