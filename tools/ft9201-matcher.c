/*
 * Offline evaluation of the FT9201 matcher.
 *
 * The program reads PGM frames, and it groups them by the name of their
 * parent directory. Each group is one finger. It then measures the matcher
 * of the driver on those frames, and it prints three results:
 *
 *   1. the matrix of the scores of each pair,
 *   2. the statistics of the same-finger and different-finger groups,
 *   3. a table of the error rates against the threshold.
 *
 * The third table shows the threshold that the driver must use. Refer to
 * FT9201_MATCH_THRESHOLD in the driver.
 *
 * The program does not hold a copy of the matcher. The file
 * ft9201-matcher-impl.inc holds the code of the driver, and the script
 * extract-matcher.sh writes that file. Thus the harness cannot measure an
 * old algorithm.
 *
 * Build:
 *   ./extract-matcher.sh
 *   gcc -O2 -o ft9201-matcher ft9201-matcher.c $(pkg-config --cflags --libs glib-2.0) -lm
 *
 * Use:
 *   ./ft9201-matcher  fingerA/frame01.pgm ...  fingerB/frame01.pgm ...
 *
 * Copyright (C) 2026 Miroslav Baranko <miroslav.baranko@upjs.sk>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include <glib.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "ft9201-matcher-impl.inc"

/* The threshold that the tables 3 and 4 use. It is the default of the
 * driver. Refer to FT9201_MATCH_THRESHOLD. */
#define EVAL_THRESHOLD 0.06f

#define MAX_FRAMES 128
#define MAX_DIM    128

typedef struct
{
  char            name[256];
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
      strcmp (magic, "P5") != 0 || maxv != 255 ||
      w < 1 || h < 1 || w > MAX_DIM || h > MAX_DIM)
    {
      fclose (fh);
      return FALSE;
    }

  fgetc (fh);
  if (fread (f->px, 1, (size_t) w * h, fh) != (size_t) w * h)
    {
      fclose (fh);
      return FALSE;
    }

  f->w = w;
  f->h = h;
  fclose (fh);
  return TRUE;
}

/* Writes the name of the parent directory of the path to the group. Frames
 * of one finger must be in one directory. */
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

/*
 * Gives the score of one probe against a template of the other frames of
 * its group. This is the operation of the driver. The driver takes the
 * second best score of the template, thus a single lucky frame cannot
 * accept a finger.
 */
static gfloat
score_against_group (Frame *frames, gint n, gint probe, const char *group)
{
  g_autoptr (GPtrArray) tpl =
    g_ptr_array_new_with_free_func ((GDestroyNotify) g_bytes_unref);
  gfloat score;

  /* ft9201_match_template reads GBytes, and not a raw pointer. */
  for (gint i = 0; i < n; i++)
    if (i != probe && strcmp (frames[i].group, group) == 0)
      g_ptr_array_add (tpl,
                       g_bytes_new (frames[i].px,
                                    (gsize) frames[i].w * frames[i].h));

  if (tpl->len < 2)
    return -1.0f;

  score = ft9201_match_template (tpl, frames[probe].px,
                                 frames[probe].w, frames[probe].h);
  return score;
}

int
main (int argc, char **argv)
{
  static Frame frames[MAX_FRAMES];
  static gfloat matrix[MAX_FRAMES][MAX_FRAMES];
  gint n = 0;

  if (argc < 3)
    {
      fprintf (stderr,
               "usage: %s  fingerA/frame*.pgm  fingerB/frame*.pgm\n"
               "The parent directory of each frame gives its group.\n",
               argv[0]);
      return 2;
    }

  for (gint a = 1; a < argc && n < MAX_FRAMES; a++)
    {
      if (!load_pgm (argv[a], &frames[n]))
        {
          fprintf (stderr, "skip %s\n", argv[a]);
          continue;
        }
      g_strlcpy (frames[n].name, argv[a], sizeof frames[n].name);
      group_of (argv[a], frames[n].group, sizeof frames[n].group);
      frames[n].feat = ft9201_features_new (frames[n].px,
                                            frames[n].w, frames[n].h);
      if (frames[n].feat == NULL)
        {
          fprintf (stderr, "no features: %s\n", argv[a]);
          continue;
        }
      n++;
    }

  printf ("%d frames\n", n);
  for (gint i = 0; i < n; i++)
    printf ("  %2d %-30s group=%-12s keypoints=%u\n", i + 1,
            strrchr (frames[i].name, '/') ?
            strrchr (frames[i].name, '/') + 1 : frames[i].name,
            frames[i].group, frames[i].feat->n);
  printf ("\n");

  /* 1. the matrix of the scores of each pair */
  printf ("pair scores\n%-26s", "");
  for (gint j = 0; j < n; j++)
    printf ("%6d", j + 1);
  printf ("\n");

  for (gint i = 0; i < n; i++)
    {
      const char *base = strrchr (frames[i].name, '/');

      printf ("%2d %-23s", i + 1, base ? base + 1 : frames[i].name);
      for (gint j = 0; j < n; j++)
        {
          matrix[i][j] = (i == j) ? 1.0f :
                         ft9201_match_pair (frames[i].feat, frames[j].feat);
          printf ("%6.3f", (gdouble) matrix[i][j]);
        }
      printf ("\n");
    }

  /* 2. the statistics of the two groups */
  {
    gdouble smin = 2, smax = -2, ssum = 0;
    gdouble dmin = 2, dmax = -2, dsum = 0;
    gint sn = 0, dn = 0;

    for (gint i = 0; i < n; i++)
      for (gint j = i + 1; j < n; j++)
        {
          gfloat v = (matrix[i][j] + matrix[j][i]) / 2.0f;

          if (strcmp (frames[i].group, frames[j].group) == 0)
            {
              sn++;
              ssum += v;
              if (v < smin) smin = v;
              if (v > smax) smax = v;
            }
          else
            {
              dn++;
              dsum += v;
              if (v < dmin) dmin = v;
              if (v > dmax) dmax = v;
            }
        }

    if (sn > 0)
      printf ("\nsame finger        n=%3d  mean %.3f  min %.3f  max %.3f\n",
              sn, ssum / sn, smin, smax);
    if (dn > 0)
      printf ("different finger   n=%3d  mean %.3f  min %.3f  max %.3f\n",
              dn, dsum / dn, dmin, dmax);
    if (sn > 0 && dn > 0 && smin > dmax)
      printf ("the two groups do not touch: %.3f to %.3f is free\n",
              dmax, smin);
  }

  /* 3. the error rates of the operation of the driver */
  {
    static gfloat genuine[MAX_FRAMES];
    static gfloat impostor[MAX_FRAMES * 8];
    gint gn = 0, in = 0;

    for (gint i = 0; i < n; i++)
      {
        gfloat s = score_against_group (frames, n, i, frames[i].group);

        if (s >= 0.0f && gn < MAX_FRAMES)
          genuine[gn++] = s;

        for (gint j = 0; j < n; j++)
          {
            if (strcmp (frames[j].group, frames[i].group) == 0)
              continue;
            s = score_against_group (frames, n, i, frames[j].group);
            if (s >= 0.0f && in < MAX_FRAMES * 8)
              impostor[in++] = s;
            break;
          }
      }

    if (gn > 0 && in > 0)
      {
        printf ("\nthe operation of the driver: each frame against a "
                "template of the other frames\n");
        printf ("  genuine   n=%3d\n  impostor  n=%3d\n", gn, in);
        printf ("\n threshold   FRR (correct finger rejected)   "
                "FAR (different finger accepted)\n");
        for (gfloat t = 0.02f; t <= 0.402f; t += 0.02f)
          {
            gint fr = 0, fa = 0;

            for (gint i = 0; i < gn; i++)
              fr += (genuine[i] < t);
            for (gint i = 0; i < in; i++)
              fa += (impostor[i] >= t);

            printf ("   %.2f       %6.1f%% (%2d/%2d)              "
                    "%6.1f%% (%2d/%2d)\n",
                    (gdouble) t, 100.0 * fr / gn, fr, gn,
                    100.0 * fa / in, fa, in);
          }
      }
    else
      {
        printf ("\nthe error-rate table needs two groups, and three or "
                "more frames in each group\n");
      }
  }

  /* 4. the effect of the number of the enrolment frames */
  {
    const char *big = NULL;
    gint bign = 0;

    for (gint i = 0; i < n; i++)
      {
        gint c = 0;

        for (gint j = 0; j < n; j++)
          if (strcmp (frames[j].group, frames[i].group) == 0)
            c++;
        if (c > bign)
          {
            bign = c;
            big = frames[i].group;
          }
      }

    if (big != NULL && bign >= 4)
      {
        printf ("\nthe number of the enrolment frames, group %s, "
                "threshold %.2f\n", big, (gdouble) EVAL_THRESHOLD);
        printf ("  frames in template   FRR (correct finger rejected)\n");

        for (gint k = 2; k < bign; k++)
          {
            gint tries = 0, rejected = 0;

            /* Each probe of the group gets a template of the k frames that
             * follow it. Thus each frame gives one measurement. */
            for (gint p = 0; p < n; p++)
              {
                g_autoptr (GPtrArray) tpl = NULL;
                gfloat s;

                if (strcmp (frames[p].group, big) != 0)
                  continue;

                tpl = g_ptr_array_new_with_free_func (
                  (GDestroyNotify) g_bytes_unref);
                for (gint step = 1; step < n && (gint) tpl->len < k; step++)
                  {
                    gint q = (p + step) % n;

                    if (strcmp (frames[q].group, big) != 0)
                      continue;
                    g_ptr_array_add (tpl,
                                     g_bytes_new (frames[q].px,
                                                  (gsize) frames[q].w *
                                                  frames[q].h));
                  }
                if ((gint) tpl->len < k)
                  continue;

                s = ft9201_match_template (tpl, frames[p].px,
                                           frames[p].w, frames[p].h);
                tries++;
                if (s < EVAL_THRESHOLD)
                  rejected++;
              }

            if (tries > 0)
              printf ("        %2d             %6.1f%% (%2d/%2d)\n",
                      k, 100.0 * rejected / tries, rejected, tries);
          }
      }
  }

  for (gint i = 0; i < n; i++)
    ft9201_features_free (frames[i].feat);

  return 0;
}
