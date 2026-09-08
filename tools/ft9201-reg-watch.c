/* Watch every register that could reveal why finger detection dies during a
 * long idle. Read-only: it never writes, so it cannot disturb a sensor that
 * another process has already armed. Run it after the driver has activated
 * the sensor (and been killed), with no finger on the sensor. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <libusb-1.0/libusb.h>

static libusb_device_handle *h;

static const unsigned char regs[] = { 0x01, 0x1d, 0x1e, 0x1f, 0x20, 0x22,
                                      0x23, 0x30, 0x41 };
#define NREGS (sizeof regs / sizeof regs[0])

int
main (int argc, char **argv)
{
  int seconds = argc > 1 ? atoi (argv[1]) : 120;
  struct timespec t0, now;
  char prev[256] = "";

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

  printf ("  t(s) 0x43");
  for (unsigned i = 0; i < NREGS; i++)
    printf ("  0x%02x", regs[i]);
  puts ("   (only changed lines are printed)");

  clock_gettime (CLOCK_MONOTONIC, &t0);
  for (;;)
    {
      char line[256];
      int n = 0;
      unsigned char st = 0xff;
      int r = libusb_control_transfer (h, 0xc0, 0x43, 0, 0, &st, 1, 2000);

      clock_gettime (CLOCK_MONOTONIC, &now);
      double t = (now.tv_sec - t0.tv_sec) + (now.tv_nsec - t0.tv_nsec) / 1e9;
      if (t > seconds)
        break;

      n += snprintf (line + n, sizeof line - n, "%s",
                     r < 0 ? " ERR" : (st ? "  01" : "  00"));
      for (unsigned i = 0; i < NREGS; i++)
        {
          unsigned char b[4] = { 0 };
          int rr = libusb_control_transfer (h, 0xc0, 0x3a, 0, regs[i], b, 4,
                                            2000);
          n += snprintf (line + n, sizeof line - n, "  %02x%02x",
                         rr < 0 ? 0xee : b[0], rr < 0 ? 0xee : b[1]);
        }
      /* Print the first sample, then only when something moved, then a
       * heartbeat every 20 s so the log shows the watch is alive. */
      if (strcmp (line, prev) != 0 || ((int) t) % 20 == 0)
        {
          printf ("%6.1f%s%s\n", t, line,
                  strcmp (line, prev) ? "  <== changed" : "");
          fflush (stdout);
          strcpy (prev, line);
        }
      usleep (900000);
    }
  puts ("done");
  return 0;
}
