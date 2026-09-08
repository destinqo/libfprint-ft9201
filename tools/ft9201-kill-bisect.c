/*
 * Which single write stops finger detection?
 *
 * The companion tool ft9201-recover-bisect answers "what brings detection
 * back". This answers the opposite and more useful question, and exists
 * because of a mistake: that tool's "full arming sequence" step wrote seven
 * registers at once, detection died, and the death was pinned on register
 * 0x30 alone. It might have been any of them.
 *
 * So: start from a state where detection is alive -- power-cycle the port and
 * download firmware first, which is the only reliable way to get there -- and
 * then apply the arming writes ONE AT A TIME, checking detection after each.
 * The first step that goes quiet names the culprit.
 *
 * A real press sets request 0x43 to 1 AND register 0x1d to 0xa0. Both are
 * required here: straight after a firmware download 0x43 sits latched at 1
 * and would otherwise be mistaken for detection.
 *
 * Keep tapping the sensor for the whole run.
 *
 * Build: gcc -O2 -o ft9201-kill-bisect ft9201-kill-bisect.c \
 *          $(pkg-config --cflags --libs libusb-1.0)
 */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <libusb-1.0/libusb.h>

#define VID 0x2808
#define PID 0x93a9
#define TIMEOUT 2000

static libusb_device_handle *h;

static int
out (unsigned char req, unsigned short val, unsigned short idx)
{
  return libusb_control_transfer (h, 0x40, req, val, idx, NULL, 0, TIMEOUT);
}

static int
rdreg (unsigned char reg)
{
  unsigned char b[4] = { 0 };
  int r = libusb_control_transfer (h, 0xc0, 0x3a, 0, reg, b, 4, TIMEOUT);

  return r < 0 ? -1 : (b[0] | (b[1] << 8));
}

static int
finger (void)
{
  unsigned char s = 0;
  int r = libusb_control_transfer (h, 0xc0, 0x43, 0, 0, &s, 1, TIMEOUT);

  return r < 0 ? -1 : s;
}

static int
watch (double seconds, int *latched_out)
{
  int polls = (int) (seconds * 1000 / 80), hits = 0, latched = 0;

  for (int i = 0; i < polls; i++)
    {
      if (finger () == 1)
        {
          if ((rdreg (0x1d) & 0xff) == 0xa0)
            hits++;
          else
            latched++;
        }
      usleep (80000);
    }
  *latched_out = latched;
  return hits;
}

static void
wake (void)
{
  out (0x22, 0x0070, 0x0070);
  usleep (16000);
  out (0x22, 0x0070, 0x0070);
  usleep (32000);
}

static void
full_arm (void)
{
  out (0x3b, 0x00, 0x22);
  out (0x3b, 0x0e, 0x23);
  out (0x3b, 0x01, 0x01);
  out (0x3b, 0x0f, 0x41);
  out (0x3b, 0xbb, 0x30);
  out (0x3b, 0x01, 0x1f);
  out (0x3b, 0x01, 0x1e);
  usleep (30000);
}

static void
read_reg_n (unsigned char reg, int n)
{
  for (int i = 0; i < n; i++)
    {
      rdreg (reg);
      usleep (2000);
    }
}

/* do_write: 0 = nothing, 1 = one register write, 2 = wake handshake,
 * 3 = the whole arming sequence, 4/5/6 = arm then read 0x20/0x30/0x1d. */
static void
step (const char *label, int do_write, unsigned char reg, unsigned char val)
{
  int hits, latched;

  if (do_write == 1)
    {
      out (0x3b, val, reg);
      usleep (30000);
    }
  else if (do_write == 2)
    {
      wake ();
    }
  else if (do_write == 3)
    {
      full_arm ();
    }
  else if (do_write >= 4)
    {
      /* The driver arms and then reads a register in a wait loop; the tools
       * that keep detection alive never do. This is the last untested
       * difference between them. */
      full_arm ();
      read_reg_n (do_write == 4 ? 0x20 : do_write == 5 ? 0x30 : 0x1d, 6);
    }
  printf ("  %-30s ", label);
  fflush (stdout);
  hits = watch (4.0, &latched);
  printf ("%-8s  hits=%-3d latched=%-3d  0x20=%04x 0x30=%04x 0x1d=%04x\n",
          hits > 0 ? "ALIVE" : "DEAD", hits, latched,
          rdreg (0x20), rdreg (0x30), rdreg (0x1d));
  fflush (stdout);
}

int
main (void)
{
  int r;

  if (libusb_init (NULL) < 0)
    return 1;
  h = libusb_open_device_with_vid_pid (NULL, VID, PID);
  if (!h)
    {
      fprintf (stderr, "device not found\n");
      return 1;
    }
  libusb_set_auto_detach_kernel_driver (h, 1);
  if ((r = libusb_claim_interface (h, 0)) < 0)
    {
      fprintf (stderr, "claim: %s\n", libusb_error_name (r));
      return 1;
    }

  puts ("Keep tapping the sensor for the whole run.");
  puts ("Run this straight after a port power-cycle plus firmware download,");
  puts ("otherwise the baseline will already be DEAD and it proves nothing.\n");

  step ("baseline (expect ALIVE)",  0, 0,    0);
  step ("after 0x22 = 0x00",        1, 0x22, 0x00);
  step ("after 0x23 = 0x0e",        1, 0x23, 0x0e);
  step ("after 0x01 = 0x01",        1, 0x01, 0x01);
  step ("after 0x41 = 0x0f",        1, 0x41, 0x0f);
  step ("after 0x30 = 0xbb",        1, 0x30, 0xbb);
  step ("after 0x1f = 0x01",        1, 0x1f, 0x01);
  step ("after 0x1e = 0x01",        1, 0x1e, 0x01);

  /*
   * Second half: from whatever state the first half left, mirror what the
   * libfprint driver's activation actually does. It opens with the 0x22 wake
   * handshake, which nothing above tested, and only then arms. If the wake
   * kills a living sensor and the arming does not bring it back, that is the
   * driver's whole problem.
   */
  puts ("");
  step ("after wake handshake 0x22",  2, 0, 0);
  step ("after full arming sequence", 3, 0, 0);
  step ("after wake, then arm again", 2, 0, 0);
  step ("  ...and arm",               3, 0, 0);

  puts ("");
  step ("arm, then read 0x20 x6",     4, 0, 0);
  step ("arm again (recover?)",       3, 0, 0);
  step ("arm, then read 0x30 x6",     5, 0, 0);
  step ("arm again (recover?)",       3, 0, 0);
  step ("arm, then read 0x1d x6",     6, 0, 0);

  puts ("\nFirst half: the first DEAD line names the write that stops detection.\n"
        "Second half: whether the wake handshake kills a living sensor, and\n"
        "whether arming afterwards revives it. That is the driver's sequence.");

  libusb_release_interface (h, 0);
  libusb_close (h);
  libusb_exit (NULL);
  return 0;
}
