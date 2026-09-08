/* FT9201 firmware-loader probe, full replay of the Windows cold-plug path.
 *
 * Reproduces, request for request, what the vendor driver sends between
 * enumeration and the first "MCU status = a5 5a", as captured in
 * usb-fingerprint.pcapng (device address 29).
 *
 * Build: gcc -O2 -o ft9201-fwload2 ft9201-fwload2.c $(pkg-config --cflags --libs libusb-1.0)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <libusb-1.0/libusb.h>

#define VID 0x2808
#define PID 0x93a9
#define EP_OUT 0x02

#define TIMEOUT 3000
#define CHUNK   64

static libusb_device_handle *h;
static int verbose = 1;

static int wr (uint8_t req, uint16_t val, uint16_t idx)
{
  int r = libusb_control_transfer (h, 0x40, req, val, idx, NULL, 0, TIMEOUT);
  if (verbose)
    printf ("  OUT 0x%02x wV=0x%04x wI=0x%04x -> %s\n", req, val, idx,
            r < 0 ? libusb_error_name (r) : "ok");
  return r;
}

static int rd (uint8_t req, uint16_t val, uint16_t idx, uint16_t len)
{
  uint8_t b[8] = { 0 };
  int r = libusb_control_transfer (h, 0xc0, req, val, idx, b, len, TIMEOUT);
  if (verbose) {
    printf ("  IN  0x%02x wV=0x%04x wI=0x%04x -> ", req, val, idx);
    if (r < 0) printf ("%s\n", libusb_error_name (r));
    else { for (int i = 0; i < r; i++) printf ("%02x ", b[i]); printf ("\n"); }
  }
  return r < 0 ? r : b[0] | (b[1] << 8);
}

static int reg20 (void)
{
  uint8_t b[4] = { 0 };
  int r = libusb_control_transfer (h, 0xc0, 0x3a, 0, 0x20, b, 4, TIMEOUT);
  if (r < 0) return -1;
  return b[0] | (b[1] << 8);
}

static void mcu_check (int gap_us)
{
  wr (0x22, 0x0070, 0x0070);
  usleep (gap_us);
  wr (0x22, 0x0070, 0x0070);
  usleep (32000);
}

int main (int argc, char **argv)
{
  const char *fwpath = argc > 1 ? argv[1] : "ft9201-fw.bin";
  int r;

  FILE *f = fopen (fwpath, "rb");
  if (!f) { perror (fwpath); return 1; }
  fseek (f, 0, SEEK_END); long fwlen = ftell (f); fseek (f, 0, SEEK_SET);
  uint8_t *fw = malloc (fwlen);
  if (fread (fw, 1, fwlen, f) != (size_t) fwlen) { perror ("read"); return 1; }
  fclose (f);
  if (fwlen % CHUNK) { fprintf (stderr, "fw not a multiple of %d\n", CHUNK); return 1; }

  if ((r = libusb_init (NULL)) < 0) { fprintf (stderr, "init: %s\n", libusb_error_name (r)); return 1; }
  h = libusb_open_device_with_vid_pid (NULL, VID, PID);
  if (!h) { fprintf (stderr, "device not found / no permission\n"); return 1; }
  libusb_set_auto_detach_kernel_driver (h, 1);
  if ((r = libusb_claim_interface (h, 0)) < 0)
    { fprintf (stderr, "claim: %s\n", libusb_error_name (r)); return 1; }

  printf ("firmware %s: %ld bytes\n", fwpath, fwlen);
  printf ("reg 0x20 before = 0x%04x\n\n", reg20 ());

  puts ("--- step 1: MCU check ---");
  rd (0x3a, 0, 0x20, 4);
  mcu_check (17000);
  rd (0x3a, 0, 0x20, 4);
  rd (0x3a, 0, 0x16, 4);
  rd (0x3a, 0, 0x17, 4);

  puts ("\n--- step 2: 0x64/0x60 block ---");
  for (int i = 0; i < 5; i++) { wr (0x64, 0x9001, 0x0000); rd (0x60, 0, 0x0000, 4); }
  wr (0x64, 0x0603, 0x00f9);

  puts ("\n--- step 3: 4-byte probe write to 0x85c0 ---");
  wr (0x34, 0x0002, 0x0000);
  wr (0x35, 0x0004, 0x85c0);
  {
    uint8_t probe[4] = { 0x11, 0xee, 0x02, 0x00 };
    int sent = 0;
    r = libusb_bulk_transfer (h, EP_OUT, probe, 4, &sent, TIMEOUT);
    printf ("  BULK OUT 4B 11ee0200 -> %s (%d)\n",
            r < 0 ? libusb_error_name (r) : "ok", sent);
  }
  usleep (9000);
  wr (0x03, 0x0001, 0x00a4);
  rd (0x30, 0, 0x85c0, 4);

  puts ("\n--- step 4: 0x57 / 0x65 / 0x66 block ---");
  wr (0x57, 0, 0);
  rd (0x65, 0, 0x00c8, 4);
  wr (0x66, 0x00df, 0x00c8);
  wr (0x66, 0x001d, 0x00f1);
  rd (0x65, 0, 0x00f4, 4);
  wr (0x66, 0x0001, 0x00f4);
  rd (0x65, 0, 0x00f3, 4);
  rd (0x1a, 0, 0, 4);

  puts ("\n--- step 5: MCU check again ---");
  mcu_check (15000);
  rd (0x3a, 0, 0x20, 4);

  puts ("\n--- step 6: 0x57 / 0x69 / 0x68 unlock, x3 ---");
  for (int i = 0; i < 3; i++) {
    wr (0x57, 0, 0);
    wr (0x69, 0x0055, 0x00c2);
    rd (0x68, 0, 0x00c2, 4);
  }

  puts ("\n--- step 7: firmware download ---");
  wr (0x34, 0x00ff, 0x0000);
  usleep (22000);
  wr (0x34, 0x0002, 0x0000);
  wr (0x35, CHUNK, 0x0000);
  verbose = 0;
  for (long off = 0; off < fwlen; off += CHUNK) {
    int sent = 0;
    r = libusb_bulk_transfer (h, EP_OUT, fw + off, CHUNK, &sent, TIMEOUT);
    if (r < 0 || sent != CHUNK) {
      fprintf (stderr, "  ! chunk at 0x%04lx: %s (%d)\n", off, libusb_error_name (r), sent);
      return 1;
    }
  }
  verbose = 1;
  printf ("  %ld x %d bytes sent\n", fwlen / CHUNK, CHUNK);

  puts ("\n--- step 8: commit ---");
  usleep (15000);
  wr (0x40, 0, 0);
  usleep (31000);
  wr (0x40, 0, 0);
  usleep (183000);

  puts ("\n--- step 9: verify ---");
  mcu_check (3000);
  int s = reg20 ();
  rd (0x3a, 0, 0x20, 4);
  rd (0x3a, 0, 0x14, 4);
  rd (0x3a, 0, 0x15, 4);
  rd (0x3a, 0, 0x16, 4);
  rd (0x3a, 0, 0x17, 4);

  if (s == 0x5aa5 || s == 0x0101)
    printf ("\n*** SUCCESS: MCU status 0x%04x - firmware is running ***\n", s);
  else
    printf ("\n*** FAILED: MCU status 0x%04x ***\n", s);

  libusb_release_interface (h, 0);
  libusb_close (h);
  libusb_exit (NULL);
  return (s == 0x5aa5 || s == 0x0101) ? 0 : 1;
}
