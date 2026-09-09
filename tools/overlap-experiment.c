/*
 * Tests a change to the matcher: divide by the keypoints in the common
 * area, and not by all keypoints.
 *
 * The matcher of the driver divides the number of agreeing pairs by the
 * number of keypoints of the smaller side. A keypoint outside the common
 * area of the two frames cannot agree with anything, thus it makes the
 * score of a partial press smaller than it must be. The matcher of the
 * vendor gives the area of the common part as a separate result, which is
 * the reason to test this.
 *
 * The program measures the matcher of the driver and the change on the
 * same frames, and it prints the two error rates next to each other.
 * Nothing here goes into the driver before the numbers say that it helps.
 *
 * Build:
 *   ./extract-matcher.sh
 *   gcc -O2 -o overlap-experiment overlap-experiment.c \
 *       $(pkg-config --cflags --libs glib-2.0) -lm
 * Use:
 *   ./overlap-experiment  ../samples/dataset-DATE/finger/frame01.pgm ...
 *
 * Copyright (C) 2026 Miroslav Baranko <miroslav.baranko@upjs.sk>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include <glib.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "ft9201-matcher-impl.inc"

#define MAX_DIM   128
#define MAX_FR    512
/* The image of the keypoints is larger than the frame by this factor. */
#define UPS       FT9201_KP_UPSCALE

/* The smallest number of keypoints in the common area. Below this number
 * the common area is too small to give a result, thus the score is 0. */
static gint g_min_overlap = 12;

/* TRUE: use the common area only as a gate, and keep the division of the
 * driver. FALSE: divide by the common area. */
static gboolean g_gate_only = FALSE;

/*
 * Gives the score of two frames, with the division by the keypoints in the
 * common area. The first part is the same as ft9201_match_pair. The
 * difference is at the end: the function keeps the best motion, then it
 * counts the keypoints of each side that the other side can see.
 */
static gfloat
match_pair_overlap (const Ft9201Features *a, const Ft9201Features *b,
                    gint w, gint h)
{
  guint pairs = 0;
  guint16 ai[FT9201_KP_MAX], bi[FT9201_KP_MAX];
  gint best_inliers = 0;
  gfloat bca = 1.0f, bsa = 0.0f, btx = 0.0f, bty = 0.0f;
  gboolean have = FALSE;
  gfloat iw = (gfloat) (w * UPS), ih = (gfloat) (h * UPS);
  gint na = 0, nb = 0, denom;

  if (a->n < 4 || b->n < 4)
    return 0.0f;

  for (guint i = 0; i < a->n && pairs < FT9201_KP_MAX; i++)
    {
      gfloat d1 = G_MAXFLOAT, d2 = G_MAXFLOAT;
      guint bestj = 0;

      for (guint j = 0; j < b->n; j++)
        {
          gfloat d = ft9201_desc_dist2 (&a->kp[i], &b->kp[j]);

          if (d < d1)
            {
              d2 = d1;
              d1 = d;
              bestj = j;
            }
          else if (d < d2)
            {
              d2 = d;
            }
        }
      if (d2 > 0.0f && d1 < FT9201_MATCH_RATIO * FT9201_MATCH_RATIO * d2)
        {
          ai[pairs] = i;
          bi[pairs] = bestj;
          pairs++;
        }
    }

  if (pairs < 3)
    return 0.0f;

  for (guint p = 0; p < pairs; p++)
    for (guint q = p + 1; q < pairs; q++)
      {
        const Ft9201Keypoint *a1 = &a->kp[ai[p]], *a2 = &a->kp[ai[q]];
        const Ft9201Keypoint *b1 = &b->kp[bi[p]], *b2 = &b->kp[bi[q]];
        gfloat adx = a2->x - a1->x, ady = a2->y - a1->y;
        gfloat bdx = b2->x - b1->x, bdy = b2->y - b1->y;
        gfloat alen = sqrtf (adx * adx + ady * ady);
        gfloat blen = sqrtf (bdx * bdx + bdy * bdy);
        gfloat ang, ca, sa, tx, ty;
        gint inliers = 0;

        if (alen < 4.0f || blen < 4.0f)
          continue;
        if (ABS (alen - blen) > FT9201_RANSAC_TOL_PX)
          continue;

        ang = atan2f (ady, adx) - atan2f (bdy, bdx);
        ca = cosf (ang);
        sa = sinf (ang);
        tx = a1->x - (b1->x * ca - b1->y * sa);
        ty = a1->y - (b1->x * sa + b1->y * ca);

        for (guint r = 0; r < pairs; r++)
          {
            const Ft9201Keypoint *ka = &a->kp[ai[r]], *kb = &b->kp[bi[r]];
            gfloat px = kb->x * ca - kb->y * sa + tx;
            gfloat py = kb->x * sa + kb->y * ca + ty;
            gfloat ex = px - ka->x, ey = py - ka->y;

            if (ex * ex + ey * ey <= FT9201_RANSAC_TOL_PX * FT9201_RANSAC_TOL_PX)
              inliers++;
          }
        if (inliers > best_inliers)
          {
            best_inliers = inliers;
            bca = ca;
            bsa = sa;
            btx = tx;
            bty = ty;
            have = TRUE;
          }
      }

  if (!have)
    return 0.0f;

  /* A keypoint of b goes to the frame of a with the best motion. It is in
   * the common area when it lands inside the image of a. */
  for (guint j = 0; j < b->n; j++)
    {
      gfloat px = b->kp[j].x * bca - b->kp[j].y * bsa + btx;
      gfloat py = b->kp[j].x * bsa + b->kp[j].y * bca + bty;

      if (px >= 0.0f && px < iw && py >= 0.0f && py < ih)
        nb++;
    }
  /* And the other direction, with the motion in reverse. */
  for (guint i = 0; i < a->n; i++)
    {
      gfloat dx = a->kp[i].x - btx, dy = a->kp[i].y - bty;
      gfloat px = dx * bca + dy * bsa;
      gfloat py = -dx * bsa + dy * bca;

      if (px >= 0.0f && px < iw && py >= 0.0f && py < ih)
        na++;
    }

  denom = MIN (na, nb);
  if (denom < g_min_overlap)
    return 0.0f;

  /* Two ways to use the common area. The first divides by it. The second
   * keeps the division of the driver and uses the common area only as a
   * gate, which is what the matcher of the vendor appears to do: it gives
   * the area as a separate result. */
  if (g_gate_only)
    return (gfloat) best_inliers / (gfloat) MIN (a->n, b->n);
  return (gfloat) best_inliers / (gfloat) denom;
}

/* ---- the evaluation ---- */

typedef struct
{
  char            group[128];
  guint8          px[MAX_DIM * MAX_DIM];
  gint            w, h;
  Ft9201Features *feat;
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

/* Gives the second best score of one probe against a template, which is
 * the rule of the driver. use_overlap selects the scorer. */
static gfloat
template_score (Frame *fr, gint n, gint probe, const char *group,
                gboolean use_overlap)
{
  gfloat first = -1.0f, second = -1.0f;
  gint scored = 0;

  for (gint i = 0; i < n; i++)
    {
      gfloat s;

      if (i == probe || strcmp (fr[i].group, group) != 0)
        continue;
      s = use_overlap ?
          match_pair_overlap (fr[i].feat, fr[probe].feat, fr[i].w, fr[i].h) :
          ft9201_match_pair (fr[i].feat, fr[probe].feat);
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
  if (scored < 2)
    return -1.0f;
  return second;
}

/* Gives the equal error rate and the separation of two sets of scores.
 * Those two numbers do not change with the scale of the score, thus they
 * compare two matchers that give different ranges. */
static void
quality (const gfloat *gen, gint gn, const gfloat *imp, gint in,
         gdouble *eer, gdouble *eer_t, gdouble *dp)
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
  *eer_t = 0.0;
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
          *eer_t = t;
        }
    }
}

static void
report (const char *title, const gfloat *gen, gint gn,
        const gfloat *imp, gint in)
{
  gdouble eer, eer_t, dp;

  quality (gen, gn, imp, in, &eer, &eer_t, &dp);
  printf ("\n%s\n", title);
  printf ("  equal error rate %.2f %% at %.3f, separation d' = %.2f\n",
          100.0 * eer, eer_t, dp);
  printf (" threshold    FRR              FAR\n");
  for (gfloat t = 0.02f; t <= 0.201f; t += 0.02f)
    {
      gint fr = 0, fa = 0;

      for (gint i = 0; i < gn; i++)
        fr += (gen[i] < t);
      for (gint i = 0; i < in; i++)
        fa += (imp[i] >= t);
      printf ("   %.2f      %5.1f%% (%3d/%3d)   %5.1f%% (%4d/%4d)\n",
              (gdouble) t, 100.0 * fr / gn, fr, gn,
              100.0 * fa / in, fa, in);
    }
}

int
main (int argc, char **argv)
{
  static Frame fr[MAX_FR];
  static gfloat gs[MAX_FR], go[MAX_FR];
  static gfloat is[MAX_FR * 32], io[MAX_FR * 32];
  gint n = 0, gn = 0, in = 0;
  const char *env = g_getenv ("MIN_OVERLAP");

  if (env != NULL)
    g_min_overlap = atoi (env);
  g_gate_only = (g_getenv ("GATE_ONLY") != NULL);

  for (gint a = 1; a < argc && n < MAX_FR; a++)
    {
      if (!load_pgm (argv[a], &fr[n]))
        continue;
      group_of (argv[a], fr[n].group, sizeof fr[n].group);
      fr[n].feat = ft9201_features_new (fr[n].px, fr[n].w, fr[n].h);
      if (fr[n].feat != NULL)
        n++;
    }
  printf ("%d frames, the smallest common area is %d keypoints\n",
          n, g_min_overlap);

  for (gint i = 0; i < n; i++)
    {
      gfloat s = template_score (fr, n, i, fr[i].group, FALSE);
      gfloat o = template_score (fr, n, i, fr[i].group, TRUE);

      if (s >= 0.0f)
        {
          gs[gn] = s;
          go[gn] = o;
          gn++;
        }
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
          s = template_score (fr, n, i, fr[j].group, FALSE);
          o = template_score (fr, n, i, fr[j].group, TRUE);
          if (s >= 0.0f && in < MAX_FR * 32)
            {
              is[in] = s;
              io[in] = o;
              in++;
            }
        }
    }

  report ("the matcher of the driver (divides by all keypoints)",
          gs, gn, is, in);
  report (g_gate_only ?
          "the change (the common area is only a gate)" :
          "the change (divides by the keypoints in the common area)",
          go, gn, io, in);

  for (gint i = 0; i < n; i++)
    ft9201_features_free (fr[i].feat);
  return 0;
}
