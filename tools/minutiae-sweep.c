/* Offline sweep: run libfprint's own minutiae detector over captured frames
 * with every combination of image flags and a range of ppmm values, and
 * report how many minutiae each combination yields. Decides empirically
 * what the FT9201 driver should declare, instead of guessing. */
#include <glib.h>
#include <stdio.h>
#include <string.h>
#include <libfprint/fprint.h>
#include "fpi-image.h"
#include <pixman.h>

typedef struct { guint w, h; guint8 *px; } Frame;

static gboolean
load_pgm (const char *path, Frame *f)
{
  gchar *data = NULL; gsize len = 0;
  if (!g_file_get_contents (path, &data, &len, NULL)) return FALSE;
  /* minimal P5 parser: "P5\n<w> <h>\n255\n" */
  const char *p = strstr (data, "255\n");
  if (!p) { g_free (data); return FALSE; }
  p += 4;
  if (sscanf (data, "P5 %u %u", &f->w, &f->h) != 2 &&
      sscanf (data, "P5\n%u %u", &f->w, &f->h) != 2) { g_free (data); return FALSE; }
  gsize n = (gsize) f->w * f->h;
  if ((gsize) (len - (p - data)) < n) { g_free (data); return FALSE; }
  f->px = g_memdup2 (p, n);
  g_free (data);
  return TRUE;
}

static int detected = -1;
static GMainLoop *loop;

static void
on_done (GObject *src, GAsyncResult *res, gpointer user)
{
  FpImage *img = FP_IMAGE (src);
  GError *e = NULL;
  if (fp_image_detect_minutiae_finish (img, res, &e))
    {
      GPtrArray *m = fp_image_get_minutiae (img);
      detected = m ? (int) m->len : 0;
    }
  else
    {
      detected = -1;
      g_clear_error (&e);
    }
  g_main_loop_quit (loop);
}

/* Byte-for-byte the same operation as libfprint's own fpi_image_resize(),
 * which is not exported from the shared library: pixman a8 bilinear scale. */
static FpImage *
local_image_resize (FpImage *orig_img, guint w_factor, guint h_factor)
{
  int new_width = orig_img->width * w_factor;
  int new_height = orig_img->height * h_factor;
  pixman_image_t *orig, *resized;
  pixman_transform_t transform;
  FpImage *newimg;

  orig = pixman_image_create_bits (PIXMAN_a8, orig_img->width, orig_img->height,
                                   (uint32_t *) orig_img->data, orig_img->width);
  resized = pixman_image_create_bits (PIXMAN_a8, new_width, new_height, NULL, new_width);

  pixman_transform_init_identity (&transform);
  pixman_transform_scale (NULL, &transform, pixman_int_to_fixed (w_factor),
                          pixman_int_to_fixed (h_factor));
  pixman_image_set_transform (orig, &transform);
  pixman_image_set_filter (orig, PIXMAN_FILTER_BILINEAR, NULL, 0);
  pixman_image_composite32 (PIXMAN_OP_SRC, orig, NULL, resized,
                            0, 0, 0, 0, 0, 0, new_width, new_height);

  newimg = fp_image_new (new_width, new_height);
  newimg->flags = orig_img->flags;
  memcpy (newimg->data, pixman_image_get_data (resized),
          (gsize) new_width * new_height);

  pixman_image_unref (orig);
  pixman_image_unref (resized);
  return newimg;
}

static int
count_minutiae (Frame *f, FpiImageFlags flags, double ppmm, guint scale)
{
  FpImage *img = fp_image_new (f->w, f->h);
  memcpy (img->data, f->px, (gsize) f->w * f->h);
  img->ppmm = ppmm;

  if (scale > 1)
    {
      FpImage *big = local_image_resize (img, scale, scale);
      g_object_unref (img);
      img = big;
      img->ppmm = ppmm * scale;
    }
  img->flags = flags;

  detected = -1;
  loop = g_main_loop_new (NULL, FALSE);
  fp_image_detect_minutiae (img, NULL, on_done, NULL);
  g_main_loop_run (loop);
  g_main_loop_unref (loop);
  g_object_unref (img);
  return detected;
}

int
main (int argc, char **argv)
{
  const struct { const char *name; FpiImageFlags f; } combos[] = {
    { "none",             FPI_IMAGE_NONE },
    { "PARTIAL",          FPI_IMAGE_PARTIAL },
    { "INVERTED",         FPI_IMAGE_COLORS_INVERTED },
    { "INVERTED|PARTIAL", FPI_IMAGE_COLORS_INVERTED | FPI_IMAGE_PARTIAL },
  };
  const double ppmms[] = { 19.685 };
  const guint scales[] = { 1, 2, 3, 4 };

  GPtrArray *frames = g_ptr_array_new ();
  for (int i = 1; i < argc; i++)
    {
      Frame *f = g_new0 (Frame, 1);
      if (load_pgm (argv[i], f)) g_ptr_array_add (frames, f);
      else { g_printerr ("skip %s\n", argv[i]); g_free (f); }
    }
  g_print ("%u frames\n\n", frames->len);

  g_print ("mean minutiae per frame / frames yielding >= 6, at ppmm %.3f\n\n",
           ppmms[0]);
  g_print ("%-18s", "flags \\ upscale");
  for (guint k = 0; k < G_N_ELEMENTS (scales); k++)
    g_print ("   %2ux (%3ux%3u)", scales[k], 96 * scales[k], 96 * scales[k]);
  g_print ("\n");

  for (guint c = 0; c < G_N_ELEMENTS (combos); c++)
    {
      g_print ("%-18s", combos[c].name);
      for (guint k = 0; k < G_N_ELEMENTS (scales); k++)
        {
          int total = 0, usable = 0;
          for (guint i = 0; i < frames->len; i++)
            {
              int n = count_minutiae (g_ptr_array_index (frames, i),
                                      combos[c].f, ppmms[0], scales[k]);
              if (n > 0) total += n;
              if (n >= 6) usable++;
            }
          g_print ("      %5.1f/%-4d", (double) total / frames->len, usable);
        }
      g_print ("\n");
    }
  return 0;
}
