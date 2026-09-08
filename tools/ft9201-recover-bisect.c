/*
 * Which recovery actually restores finger detection?
 *
 * The sensor intermittently stops answering vendor request 0x43 with 01, and
 * then nothing detects a finger -- not the libfprint driver, not a bare
 * poller, and not any particular action. Testing candidate recoveries one
 * per run costs a whole press session each and has produced contradictory
 * results. This tries them all in one run instead: apply a candidate, poll
 * 0x43 for a few seconds, report whether detection came back, move on.
 *
 * Keep tapping the sensor throughout. A step reporting 0 hits means that
 * recovery did not work; the first step reporting hits is the answer.
 *
 * Build: gcc -O2 -o ft9201-recover-bisect ft9201-recover-bisect.c \
 *          $(pkg-config --cflags --libs libusb-1.0)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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

/*
 * Poll for the given seconds and count real finger reports.
 *
 * Request 0x43 alone is not enough to judge by: right after a firmware
 * download, and before the capture engine is armed, it sits latched at 1 and
 * answers 1 to every poll whether or not anything is touching the sensor. An
 * earlier version of this tool counted those and cheerfully reported
 * "detection back" on 50 of 50 polls. A real press also puts 0xa0 in
 * register 0x1d, which is what the driver requires, so require both.
 */
static int
watch (double seconds)
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
  if (latched > polls / 2)
    printf ("[0x43 latched high, %d polls] ", latched);
  return hits;
}

static void a_none (void) { }

static void
a_wake (void)
{
  out (0x22, 0x0070, 0x0070);
  usleep (16000);
  out (0x22, 0x0070, 0x0070);
  usleep (32000);
}

static void
a_autopower (void)
{
  out (0x3b, 0x01, 0x1f);
  out (0x3b, 0x01, 0x1e);
  usleep (20000);
}

static void
a_sensor_cfg (void)
{
  /* the two registers the vendor writes on every activation */
  out (0x3b, 0x00, 0x22);
  out (0x3b, 0x0e, 0x23);
  usleep (20000);
}

static void
a_full_arm (void)
{
  a_wake ();
  out (0x3b, 0x00, 0x22);
  out (0x3b, 0x0e, 0x23);
  out (0x3b, 0x01, 0x01);
  out (0x3b, 0x0f, 0x41);
  out (0x3b, 0xbb, 0x30);
  out (0x3b, 0x01, 0x1f);
  out (0x3b, 0x01, 0x1e);
  usleep (20000);
}

static void
a_bulk_reset (void)
{
  out (0x34, 0x00ff, 0x0000);
  usleep (20000);
}

static void
a_usb_reset (void)
{
  libusb_reset_device (h);
  usleep (300000);
  libusb_claim_interface (h, 0);
}

static void
step (const char *name, void (*apply) (void), double seconds)
{
  int hits;

  printf ("  %-32s ", name);
  fflush (stdout);
  apply ();
  hits = watch (seconds);
  printf ("%-15s (%2d hits, reg 0x20=%04x 0x30=%04x)\n",
          hits > 0 ? "DETECTION BACK" : "nothing", hits,
          rdreg (0x20), rdreg (0x30));
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

  puts ("Keep tapping the sensor for the whole run.\n");
  printf ("  at start: reg 0x20=%04x reg 0x30=%04x req 0x43=%02x\n\n",
          rdreg (0x20), rdreg (0x30), finger ());

  step ("baseline, nothing applied",   a_none,       4.0);
  step ("wake handshake 0x22 x2",      a_wake,       4.0);
  step ("auto-power 0x1f/0x1e",        a_autopower,  4.0);
  step ("sensor config 0x22/0x23",     a_sensor_cfg, 4.0);
  step ("full arming sequence",        a_full_arm,   5.0);
  step ("bulk engine reset 0x34 ff",   a_bulk_reset, 4.0);
  step ("USB device reset",            a_usb_reset,  5.0);

  puts ("\nIf every step says nothing, the recovery is not on this list. Try a port\n"
        "power cycle (uhubctl) and run this again to see whether that alone helps.");

  libusb_release_interface (h, 0);
  libusb_close (h);
  libusb_exit (NULL);
  return 0;
}
