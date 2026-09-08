/* Reproduce the idle stall outside the driver and watch both finger
 * signals through it.
 *
 * The question this answers: when request 0x43 has stopped reporting a
 * press, does register 0x1d still see the finger? If it does, the AFE is
 * still imaging and only the interrupt port is dead, and the driver can
 * poll both. If it does not, the analogue side has stopped too.
 *
 * Run it on a sensor the driver has already armed (and then been killed),
 * press once at the start and once near the end. */
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include <libusb-1.0/libusb.h>

#define POLL_US        80000
#define KEEPALIVE_POLLS 1000

static libusb_device_handle *h;

static int
rdreg (unsigned char reg, unsigned char out[4])
{
  return libusb_control_transfer (h, 0xc0, 0x3a, 0, reg, out, 4, 2000);
}

static int
wrreg (unsigned char reg, unsigned char val)
{
  return libusb_control_transfer (h, 0x40, 0x3b, val, reg, NULL, 0, 2000);
}

static int
wake (void)
{
  return libusb_control_transfer (h, 0x40, 0x22, 0x0070, 0x0070, NULL, 0,
                                  2000);
}

static double
now (struct timespec *t0)
{
  struct timespec t;

  clock_gettime (CLOCK_MONOTONIC, &t);
  return (t.tv_sec - t0->tv_sec) + (t.tv_nsec - t0->tv_nsec) / 1e9;
}

/* The vendor's renewal, including the three reads it takes afterwards. */
static void
renew (struct timespec *t0)
{
  unsigned char b[4] = { 0 }, c[4] = { 0 }, d[4] = { 0 };

  wake ();
  usleep (16000);
  wake ();
  usleep (48000);
  rdreg (0x20, b);
  wrreg (0x1f, 1);
  wrreg (0x1e, 1);
  usleep (14000);
  rdreg (0x20, c);
  rdreg (0x1d, d);
  rdreg (0x20, c);
  printf ("%7.1f  renewal: 0x20 before=%02x%02x  0x1d=%02x%02x  "
          "0x20 after=%02x%02x %s\n", now (t0), b[0], b[1], d[0], d[1],
          c[0], c[1], c[0] == 0x01 ? "(hunting)" : "(IDLE -- dead)");
  fflush (stdout);
}

int
main (int argc, char **argv)
{
  int seconds = argc > 1 ? atoi (argv[1]) : 150;
  struct timespec t0;
  unsigned idle = 0;
  int last43 = -1, last1d = -1;

  if (libusb_init (NULL) < 0)
    return 1;
  h = libusb_open_device_with_vid_pid (NULL, 0x2808, 0x93a9);
  if (!h)
    {
      fprintf (stderr, "no device\n");
      return 1;
    }
  libusb_set_auto_detach_kernel_driver (h, 1);
  if (libusb_claim_interface (h, 0) < 0)
    {
      fprintf (stderr, "claim failed -- is the driver still running?\n");
      return 1;
    }

  puts ("   t(s)  event");
  clock_gettime (CLOCK_MONOTONIC, &t0);

  for (;;)
    {
      unsigned char st = 0, d[4] = { 0 };
      double t = now (&t0);

      if (t > seconds)
        break;

      if (libusb_control_transfer (h, 0xc0, 0x43, 0, 0, &st, 1, 2000) < 0)
        {
          printf ("%7.1f  0x43 transfer failed\n", t);
          fflush (stdout);
          usleep (POLL_US);
          continue;
        }
      rdreg (0x1d, d);

      if (st != last43 || d[0] != last1d)
        {
          printf ("%7.1f  0x43=%02x  0x1d=%02x%02x%s\n", t, st, d[0], d[1],
                  (st == 0 && d[0] == 0xa0)
                  ? "   <== AFE SEES THE FINGER, interrupt port does not"
                  : "");
          fflush (stdout);
          last43 = st;
          last1d = d[0];
        }

      idle = st ? 0 : idle + 1;
      if (idle >= KEEPALIVE_POLLS)
        {
          idle = 0;
          renew (&t0);
        }
      usleep (POLL_US);
    }
  puts ("done");
  return 0;
}
