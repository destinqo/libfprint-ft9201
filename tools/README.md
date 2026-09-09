# Development tools

None of this is part of the driver or the RPM. These are the programs the
protocol was worked out with, kept because they are much faster to iterate
with than a full `fprintd` cycle, and because the measurements in the driver
README were made with them.

## Talking to the hardware directly

Build any of them with:

```bash
gcc -O2 -Wall -o <name> <name>.c $(pkg-config --cflags --libs libusb-1.0) -lm
```

They all need write access to the USB device, so run them under `sudo`.

- **`ft9201-fwload2.c`** — replays the vendor's cold-plug firmware download
  request for request, printing every transfer and the register values before
  and after. This is what proved the sensor has no persistent firmware. Takes
  the firmware image as its argument. Useful for checking whether a given
  firmware revision boots at all:
  `sudo ./ft9201-fwload2 /lib/firmware/focaltech/ft9201.bin`
- **`ft9201-capture.c`** — arms the sensor and grabs N frames to
  `linux-frameNN.pgm`, with the same three gates the driver uses (request
  `0x43`, register `0x30` = `0xbb`, register `0x1d` = `0xa0`) and the same
  `dd dd` header check. Prints per-frame statistics.
  `sudo ./ft9201-capture 6`
- **`ft9201-probe-finger.c`** — dumps request `0x43` and registers `0x1d` /
  `0x20` every 100 ms across a press and a lift, with libusb error codes
  checked separately from zero data. This is what showed that the AFE powers
  itself down and answers all-zeroes, which is not the same thing as having
  lost its firmware.

## Offline analysis, no hardware needed

- **`ft9201-matcher.c`** — measures the matcher of the driver on saved
  frames. **This is the program to use when validating
  `FT9201_MATCH_THRESHOLD`.** No hardware is needed.

  The program holds no copy of the matcher. `extract-matcher.sh` takes the
  matcher out of `focaltech_ft9201.c` between the section markers
  `/****** MATCHER ******/` and `/****** TEMPLATE STORAGE ******/`, and
  writes `ft9201-matcher-impl.inc`. The harness includes that file. An
  earlier version held a copy, and that copy became old: it measured the
  whole-frame correlation that the driver dropped.

  ```bash
  ./extract-matcher.sh
  gcc -O2 -Wall -Wextra -o ft9201-matcher ft9201-matcher.c \
      $(pkg-config --cflags --libs glib-2.0) -lm
  ./ft9201-matcher  fingerA/frame*.pgm  fingerB/frame*.pgm
  ```

  Put the frames of one finger in one directory. The name of the parent
  directory gives the group. The program prints four results:

  1. the score of each pair, with the number of the keypoints per frame,
  2. the statistics of the same-finger and different-finger groups,
  3. the error rates against the threshold, for the operation of the
     driver — each frame against a template of the other frames of its
     group, with the second-best rule of `ft9201_match_template`,
  4. the error rate against the number of the frames in the template. This
     answers the question if `FT9201_ENROLL_STAGES` is large enough.

  **Collect the frames as the sensor is used.** Put the finger down the
  same way each time. Do not change the position on purpose. The sensor is
  3 x 4 mm, thus a changed position records a different area of the same
  finger, and two different areas do not correlate.
- **`minutiae-sweep.c`** — runs libfprint's own NBIS minutiae detector over
  frames with every combination of image flags, `ppmm` and 1x–4x upscaling.
  This is what established that a 96x96 frame yields 1–2 minutiae no matter
  what, which is why the driver does not use the minutiae matcher. Needs
  libfprint's private headers:
  ```bash
  L=../../work/libfprint-v1.94.100
  gcc -O2 -o minutiae-sweep minutiae-sweep.c \
      -I$L -I$L/libfprint -I$L/build -I$L/build/libfprint \
      $(pkg-config --cflags glib-2.0 gio-2.0 gusb pixman-1) \
      -L$L/build/libfprint -lfprint-2 \
      $(pkg-config --libs glib-2.0 gio-2.0 gobject-2.0 pixman-1)
  LD_LIBRARY_PATH=$L/build/libfprint ./minutiae-sweep ../samples/*.pgm
  ```
- **`enhance.py`** — writes enhanced variants of the sample frames
  (histogram equalisation, CLAHE, unsharp, ridge band-pass and combinations)
  so `minutiae-sweep` can be run over each. Produced the negative result that
  enhancement raises the minutiae count only from 1.1 to 1.5.
- **`mosaic.py`** — measures how far apart consecutive presses land, by phase
  correlation. Produced the negative result that stitching frames gains no
  area. Note its shift sign convention is inverted; the correlation values
  are what matter.
- **`simmatch.py`** — the Python prototype of the matcher, kept because it is
  quicker to try algorithm changes in than the C version. The C version
  scores better; if you change the algorithm, change it here first and then
  port it.

Both Python scripts need only `numpy`.

## `ft9201-reg-watch.c`

Read-only register watch for the idle stall. Reads `0x01`, `0x1d`, `0x1e`,
`0x1f`, `0x20`, `0x22`, `0x23`, `0x30`, `0x41` and request `0x43` once a
second and prints a line only when something changed (plus a heartbeat every
20 s). It never writes, so it cannot disturb a sensor that the driver has
already armed.

```bash
gcc -O2 -o /tmp/ft9201-reg-watch ft9201-reg-watch.c $(pkg-config --cflags --libs libusb-1.0)
sudo uhubctl -l 5-5 -p 3 -a cycle -d 4
sudo timeout 12 .../examples/img-capture /tmp/x.pgm   # let the driver arm it
sudo /tmp/ft9201-reg-watch 150                        # then watch, no finger
```

The question it exists to answer: is there any observable that separates
"armed and waiting" from "detection dead"? Without one, every experiment on
the stall costs a human finger press.

## `ft9201-idle-hunt.c`

Reproduces the idle stall outside the driver and watches both finger
signals through it: it polls request `0x43` and register `0x1d` together
every 80 ms and performs the vendor's renewal every 1000 idle polls,
printing a line whenever either signal changes and flagging the case that
matters -- `0x43` = `00` while `0x1d` = `a0`, meaning the AFE still sees
the finger and only the interrupt port has gone quiet. If that ever
prints, the driver can poll both signals instead of needing a power cycle.

```bash
gcc -O2 -o /tmp/ft9201-idle-hunt ft9201-idle-hunt.c $(pkg-config --cflags --libs libusb-1.0)
sudo uhubctl -l 5-5 -p 3 -a cycle -d 4
sudo timeout 12 .../examples/img-capture /tmp/x.pgm   # let the driver arm it
sudo /tmp/ft9201-idle-hunt 150                        # press once early, once near the end
```

- **`rotation-test.c`** — measures the effect of a turn on the matcher of
  the driver. It turns each frame and scores the turned frame against the
  frame that is not turned. It uses the same include file as
  `ft9201-matcher`, thus it measures the code of the driver.

  ```bash
  ./extract-matcher.sh
  gcc -O2 -o rotation-test rotation-test.c \
      $(pkg-config --cflags --libs glib-2.0) -lm
  ./rotation-test  ../samples/dataset-*/*/*.pgm
  ```

  A turn of 90 degrees needs no interpolation, thus its score shows what
  the matcher does with a turn alone. An angle between 0 and 90 degrees
  also carries the loss from the bilinear interpolation.

## The audible cues

A hardware test needs a person, and that person does not watch the terminal.
Two scripts give a sound. They are different, because the two messages are
different:

- **`beep`** — three bell sounds. It says "put your finger on the sensor
  now". A test uses it before each operation that waits for a finger.
- **`attention`** — two message sounds. It says "read the terminal". It does
  not ask for a finger.

Use one bell for "the press was accepted, press again". The enrolment test
does this, because 15 presses with no answer are difficult to count.
