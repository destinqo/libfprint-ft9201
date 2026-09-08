/* Watch what the finger-presence signals do across a press and a lift. */
#include <stdio.h>
#include <unistd.h>
#include <libusb-1.0/libusb.h>

static libusb_device_handle *h;

static int rdreg (unsigned char reg, unsigned char out[4])
{
  return libusb_control_transfer (h, 0xc0, 0x3a, 0, reg, out, 4, 2000);
}

int main (void)
{
  unsigned char b[4], st = 0;
  if (libusb_init (NULL) < 0) return 1;
  h = libusb_open_device_with_vid_pid (NULL, 0x2808, 0x93a9);
  if (!h) { fprintf (stderr, "no device\n"); return 1; }
  libusb_set_auto_detach_kernel_driver (h, 1);
  if (libusb_claim_interface (h, 0) < 0) { fprintf (stderr, "claim failed\n"); return 1; }

  puts ("t(s)   req0x43  reg0x1d      reg0x20");
  for (int i = 0; i < 130; i++) {
      st = 0xff;
      unsigned char d[4] = {0}, s[4] = {0};
      int r43 = libusb_control_transfer (h, 0xc0, 0x43, 0, 0, &st, 1, 2000);
      int r1d = rdreg (0x1d, d);
      int r20 = rdreg (0x20, s);
      /* Distinguish "the device answered with zeroes" from "the transfer
       * failed" -- the previous version of this probe conflated the two. */
      if (r43 < 0 || r1d < 0 || r20 < 0) {
          printf ("%5.1f    USB ERROR  0x43=%s 0x1d=%s 0x20=%s\n", i * 0.1,
                  r43 < 0 ? libusb_error_name (r43) : "ok",
                  r1d < 0 ? libusb_error_name (r1d) : "ok",
                  r20 < 0 ? libusb_error_name (r20) : "ok");
      } else {
          printf ("%5.1f    %02x      %02x %02x        %02x %02x\n",
                  i * 0.1, st, d[0], d[1], s[0], s[1]);
      }
      fflush (stdout);
      usleep (100000);
  }
  (void) b;
  libusb_release_interface (h, 0);
  libusb_close (h);
  libusb_exit (NULL);
  return 0;
}
