/* FT9201 capture probe: arms the sensor and grabs frames the way the Windows
 * vendor driver does - finger presence from vendor request 0x43, not from
 * register 0x1d, and a single 9218-byte bulk read (2-byte header + 96x96).
 * Assumes MCU firmware is already resident (run ft9201-fwload2 first).
 *
 * Build: gcc -O2 -o ft9201-capture ft9201-capture.c $(pkg-config --cflags --libs libusb-1.0)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <math.h>
#include <libusb-1.0/libusb.h>

#define VID 0x2808
#define PID 0x93a9
#define EP_IN 0x83
#define TIMEOUT 3000

#define W 96
#define H 96
#define HDR 2
#define FRAME (HDR + W * H)

static libusb_device_handle *h;

static int wr (uint8_t req, uint16_t val, uint16_t idx)
{ return libusb_control_transfer (h, 0x40, req, val, idx, NULL, 0, TIMEOUT); }

static int rdreg (uint8_t reg)
{
  uint8_t b[4] = { 0 };
  int r = libusb_control_transfer (h, 0xc0, 0x3a, 0, reg, b, 4, TIMEOUT);
  return r < 0 ? -1 : (b[0] | (b[1] << 8));
}

static int finger_state (void)
{
  uint8_t s = 0;
  int r = libusb_control_transfer (h, 0xc0, 0x43, 0, 0, &s, 1, TIMEOUT);
  return r < 0 ? -1 : s;
}

/* Register 0x20 reports a5 5a only just after this handshake on some
 * firmware revisions; bare re-reads sit at 01 01 forever. */
static void wake (void)
{
  wr (0x22, 0x0070, 0x0070);
  usleep (16000);
  wr (0x22, 0x0070, 0x0070);
  usleep (32000);
}

static int wait_ready (int tries)
{
  for (int i = 0; i < tries; i++)
    {
      wake ();
      if (rdreg (0x20) == 0x5aa5)
        return 1;
    }
  return 0;
}

static void arm (void)
{
  /* The vendor driver writes 0x22/0x23 on every activation but the three
   * arming registers only when 0x30 is not already 0xbb. Writing them to an
   * already-running sensor wedges it: register 0x20 then sits at 01 01 and
   * request 0x43 never reports a finger again. */
  wr (0x3b, 0x00, 0x22);
  wr (0x3b, 0x0e, 0x23);
  if ((rdreg (0x30) & 0xff) != 0xbb)
    {
      wr (0x3b, 0x01, 0x01);
      wr (0x3b, 0x0f, 0x41);
      wr (0x3b, 0xbb, 0x30);
      printf ("  armed; reg 0x30 = 0x%04x (want 0x00bb)\n", rdreg (0x30));
    }
  else
    {
      printf ("  already armed (reg 0x30 = 0x00bb)\n");
    }
  wr (0x3b, 0x01, 0x1f);
  wr (0x3b, 0x01, 0x1e);
  usleep (20000);
}

static void rearm (void)
{
  rdreg (0x20);
  wr (0x3b, 0x01, 0x1f);
  wr (0x3b, 0x01, 0x1e);
  usleep (20000);
}

int main (int argc, char **argv)
{
  int want = argc > 1 ? atoi (argv[1]) : 3;
  /* Seconds to wait per frame. Short by default: a stuck frame should cost
   * a few seconds, not minutes. */
  int wait_s = argc > 2 ? atoi (argv[2]) : 20;
  int max_polls = wait_s * 1000 / 80;
  int r;

  if ((r = libusb_init (NULL)) < 0) { fprintf (stderr, "init: %s\n", libusb_error_name (r)); return 1; }
  h = libusb_open_device_with_vid_pid (NULL, VID, PID);
  if (!h) { fprintf (stderr, "device not found / no permission\n"); return 1; }
  libusb_set_auto_detach_kernel_driver (h, 1);
  if ((r = libusb_claim_interface (h, 0)) < 0)
    { fprintf (stderr, "claim: %s\n", libusb_error_name (r)); return 1; }

  /* Wake before judging. An AFE that has powered itself down answers every
   * register read with zeroes, which looks exactly like missing firmware --
   * so the handshake has to come first or an idle sensor gets misdiagnosed.
   * Register reads taken while the MCU is busy return the 01 01 status
   * pattern rather than data, hence waiting for the ready state too. */
  printf ("reg 0x20 = 0x%04x (before wake)\n", rdreg (0x20));
  if (!wait_ready (50))
    {
      fprintf (stderr, "MCU is not answering even after a wake handshake "
               "(register 0x20 reads 0x%04x) - load the firmware first\n",
               rdreg (0x20));
      return 1;
    }
  printf ("reg 0x20 settled at 0x%04x\n", rdreg (0x20));
  printf ("dimensions: %d x %d, chip id 0x%04x\n",
          rdreg (0x14) & 0xff, rdreg (0x15) & 0xff,
          ((rdreg (0x16) & 0xff) << 8) | (rdreg (0x17) & 0xff));

  puts ("arming...");
  arm ();

  uint8_t *buf = malloc (FRAME);
  for (int n = 1; n <= want; n++) {
    printf ("\n[%d/%d] waiting for finger (vendor request 0x43)...\n", n, want);
    int polls = 0, spurious = 0;
    for (;;) {
      usleep (80000);
      if (++polls > max_polls)
        { printf ("  timeout (%d s)\n", wait_s); goto done; }
      /* Guard 1: the MCU must be idle. Reads issued while it is busy come
       * back as the 01 01 status pattern instead of real register data. */
      /* Poll request 0x43 unconditionally, which is exactly what the vendor
       * driver does: in the Windows capture the wait loop reads nothing but
       * 0x43 every 80 ms and never touches register 0x20. Gating this on
       * "MCU ready" was an invention, and a harmful one -- the AFE sits
       * powered down between presses, so the gate never opened and the
       * finger was never seen. */
      if (finger_state () != 1)
        continue;
      /* Guard 2: the capture engine must actually be armed. */
      int r30 = rdreg (0x30), r1d = rdreg (0x1d);
      /* Guard 3: 0xa0 in register 0x1d is the only value that accompanies a
       * real frame; 0x01 is the busy pattern bleeding through. */
      if ((r30 & 0xff) != 0xbb || (r1d & 0xff) != 0xa0) {
        spurious++;
        continue;
      }
      printf ("  finger detected after %d polls (%d spurious); reg 0x30=0x%04x reg 0x1d=0x%04x\n",
              polls, spurious, r30, r1d);
      break;
    }

    wr (0x34, 0x00ff, 0x0000);
    usleep (15000);
    wr (0x34, 0x0003, 0x0000);
    wr (0x35, FRAME, 0x3400);

    int got = 0;
    r = libusb_bulk_transfer (h, EP_IN, buf, FRAME, &got, TIMEOUT);
    if (r < 0) { printf ("  bulk IN failed: %s (%d bytes)\n", libusb_error_name (r), got); rearm (); continue; }
    printf ("  bulk IN: %d bytes, header %02x %02x\n", got, buf[0], buf[1]);
    if (got != FRAME) { printf ("  short frame, skipping\n"); rearm (); continue; }
    /* Guard 4: a real frame is prefixed dd dd. Anything else means the MCU
     * answered with status bytes rather than pixels. */
    if (buf[0] != 0xdd || buf[1] != 0xdd) {
      printf ("  bad frame header, skipping\n"); rearm (); continue;
    }

    unsigned char *px = buf + HDR;
    int mn = 255, mx = 0; long sum = 0; int seen[256] = { 0 }, distinct = 0;
    for (int i = 0; i < W * H; i++) {
      if (px[i] < mn) mn = px[i];
      if (px[i] > mx) mx = px[i];
      sum += px[i];
      if (!seen[px[i]]++) distinct++;
    }
    double mean = (double) sum / (W * H), var = 0;
    for (int i = 0; i < W * H; i++) var += (px[i] - mean) * (px[i] - mean);
    int trans = 0;
    for (int y = 0; y < H; y++) {
      long rs = 0;
      for (int x = 0; x < W; x++) rs += px[y * W + x];
      double rm = (double) rs / W;
      for (int x = 1; x < W; x++)
        if ((px[y * W + x - 1] > rm) != (px[y * W + x] > rm)) trans++;
    }
    printf ("  min=%d max=%d distinct=%d stdev=%.1f transitions/row=%.1f\n",
            mn, mx, distinct, sqrt (var / (W * H)), (double) trans / H);

    char path[64];
    snprintf (path, sizeof path, "linux-frame%02d.pgm", n);
    FILE *f = fopen (path, "wb");
    fprintf (f, "P5\n%d %d\n255\n", W, H);
    fwrite (px, 1, W * H, f);
    fclose (f);
    printf ("  wrote %s\n", path);

    rearm ();
  }
done:
  libusb_release_interface (h, 0);
  libusb_close (h);
  libusb_exit (NULL);
  return 0;
}
