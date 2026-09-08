# How to help

The most useful contributions need hardware, and they are these:

1. **A test on a different unit.** The driver has tests on one unit only,
   with the AFE chip ID `0x95a8`. The paths for the IDs `0x9338` and
   `0x9536` have no test. Report the ID that `journalctl -u fprintd`
   gives, and the sensor dimensions.
2. **Match scores from more fingers and more persons.** The threshold
   `FT9201_MATCH_THRESHOLD` has data from two fingers of one person. That
   is not sufficient for an authentication parameter. The README section
   "Verification: how it operates" gives the method and the numbers.
3. **A firmware revision with dimensions larger than 96x96.** Two
   revisions are known. A larger image is the one condition that helps
   this sensor most.

## The rules for the code

The driver goes to libfprint. Thus it follows the rules of libfprint:

- Use the code style of libfprint. Their CI runs
  `scripts/uncrustify.sh --check` with `scripts/uncrustify.cfg`, and this
  driver passes it. Run the same check before you send a change.
- The license is LGPL-2.1 or later, as in libfprint.
- Write the comments and the documents in ASD-STE100 English: short
  sentences, active voice, no idioms.
- Give a measurement for each claim about the hardware. This project
  reversed several conclusions that had an argument but no measurement.

## The firmware

This repository holds no firmware image, and it cannot hold one. The image
is the property of FocalTech. The tool `ft9201-extract-firmware.py` gets
the image on your own machine: with `--download` from the public Microsoft
Update Catalog, or from a local vendor binary. Do not send a firmware
image, a USB capture of the vendor driver, or a fingerprint image to this
repository.
