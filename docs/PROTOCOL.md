# The protocol and the hardware of the FT9201

This file holds the technical notes for the driver. The file
[`../README.md`](../README.md) tells you how to build and use it, and
[`MATCHING.md`](MATCHING.md) tells you how the verification operates.

## Confirmed on the hardware

The tests used a physical `2808:93a9` unit with `bcdDevice 0100`.

- **The protocol operates, also from a cold start.** The test removed the
  power from the sensor, thus the sensor had no firmware. The driver then
  sent the firmware, read the chip data, armed the sensor and captured a
  96x96 fingerprint. The download needs approximately 700 ms.
- **A complete enrolment passes.** It has 15 stages, and it needed 19
  seconds on the hardware. An earlier version had 8 stages. A measurement
  of the rate of the incorrect reject operations gave the larger value.
  Refer to `FT9201_ENROLL_STAGES`.
- **The firmware download is necessary, and earlier drivers did not have
  it.** The bulk OUT endpoint `0x02` is not only for a firmware update.
  10368 bytes of 8051 code go through it after each cold connection.
- **The AFE chip ID is `0x95a8`.** The reverse engineering project gives a
  full sequence only for this ID. Thus the test unit does not use the paths
  for `0x9338` and `0x9536`.
- **The sensor dimensions depend on the firmware revision.** Therefore the
  driver reads them at each activation, from the AFE registers `0x14` and
  `0x15`. It does not use constant values. The image from the Windows driver
  gives **96x96**. The image from the Linux driver of FocalTech gives
  **64x80**. Both images operate on the same unit. Thus the value
  "approximately 64x80" in the reverse engineering was correct for a
  different firmware. Use the 96x96 image if you can: more pixels is the one
  condition that helps this sensor.
- **Two firmware revisions operate from end to end.** This also showed a
  fault in the driver. With the 64x80 revision, the register `0x20` stays at
  `01 01` for all time. It gives `a5 5a` only immediately after a wake
  handshake. Thus the "wait until ready" loop must send that handshake again
  between two reads. The driver now does this.
- **The vendor request `0x43` reports a finger, and not the register
  `0x1d`.** The vendor driver sends `0x43` each 80 ms. The request has
  `wLength` 1, and the answer is `00` or `01`. The vendor driver never polls
  `0x1d` for this purpose. The earlier driver had a name for `0x43` and
  never sent it.
- **The driver must write `0xbb` to the register `0x30`.** The sensor does
  not set this value. The vendor driver writes it to arm the capture engine,
  together with `0x01` in the register `0x01` and `0x0f` in the register
  `0x41`. A driver that only reads this register sees `0x00` for all time,
  and makes the incorrect conclusion that the hardware has a fault.
- **The value `0x01` in the register `0x1d` is not a finger.** Only `0xa0`
  occurs with a true frame. The value `0x01` is the low byte of the busy
  pattern `01 01` of the MCU. A driver that accepts it gets a frame of
  `0x00` and `0x01` bytes, and not an image. The earlier driver accepted
  both values.
- **A read during the busy state gives `01 01` and not data.** An early read
  of the dimensions makes a correct 96x96 sensor report a size of 1x1. Thus
  each read must wait for the value `a5 5a` in the register `0x20`.
- **Each image transfer starts with `dd dd`.** A frame with a different
  start holds status bytes and not pixels, and the driver discards it. The
  transfer is one bulk IN read of 9218 bytes: 2 header bytes and 96x96
  pixels.
- **The vendor driver writes the registers `0x22` and `0x23` at each
  activation**, warm or cold. It does not write them only when the
  dimensions differ from the default. The earlier driver made these writes
  conditional, thus it never made them.
- **Endpoints.** The bulk IN endpoint is `0x83`, with a maximum packet size
  of 32 bytes. The bulk OUT endpoint is `0x02`, with a maximum packet size
  of 16 bytes. The interface class is `ff/ff/ff`. The device operates at
  full speed, 12 Mbps, and no kernel driver uses it.
- **The `fprintd` integration operates.** `fprintd-list` gives the name of
  the device, the scan type `press`, and a correct Polkit authorization.
- **The image area of the sensor is a square of approximately 4.5 mm.** This
  is a measurement and not an estimate. The primary ridge wavelength in 23
  frames is 9.6 to 13.7 pixels, with a median of 10.7. At a ridge distance
  of 0.4 to 0.5 mm this gives 21 to 27 pixels for each millimetre, thus
  approximately 500 dpi. The driver gives this value as `ppmm`. libfprint
  keeps `ppmm` at 0 if a driver does not set it.


## Not confirmed

- **The match threshold on more than one person.** The section
  [`MATCHING.md`](MATCHING.md) gives the measurements and their limits.
  This is the one value in the driver that needs more data, because it is an
  authentication parameter.
- **A reliable report of the finger removal.** The driver reports that the
  finger is away immediately after it has the frame. It does not wait for
  the true removal, because no signal for "the finger is still on the
  sensor" is known. The request `0x43` and the register `0x1d` both stop
  their report approximately half a second after a press. This occurs also
  when the finger stays on the sensor. In an enrolment this is not a
  problem, because each stage is a new press. But one long press can
  complete more than one stage.
- The paths for the chip IDs `0x9338` and `0x9536`. Also the values of the
  registers `0x22` and `0x23` on a unit with other dimensions.
- The function of the download requests `0x03`, `0x30`, `0x57`, `0x60`,
  `0x64`, `0x65`, `0x66`, `0x68` and `0x69`. The driver sends them again in
  their captured form, because the download fails without them. Their effect
  on the chip is not known. A download with only `0x34`, `0x35`, the image
  and `0x40` does not operate.


## Open questions

The items that the tests confirmed are in the section above. These questions have no answer:

- **The function of the undocumented download requests.** The driver sends
  `0x03`, `0x30`, `0x57`, `0x60`, `0x64`, `0x68` and `0x69` in their
  captured form. The pair `0x65`/`0x66` is a byte read and write operation in the SFR space
  of the 8051 core. The section "What other projects found" gives the two
  locations that carry data. The pair
  `0x68`/`0x69` does the same in the code RAM. The three writes of `0x55` to
  the location `0xc2` have the appearance of an unlock operation. The
  request `0x57` needs approximately 12.5 ms, thus it probably writes
  something to a permanent memory. No test confirms these ideas. A download
  without these requests fails, thus they have a function.
- **The probe word `11 ee 02 00`** that the driver writes to the address
  `0x85c0` before the download. After that write, the request `0x30` gives
  `0x50 0x2b`. The function is not known, and the download fails without
  this step.
- **The last 48 bytes of the 10368 bytes are not firmware.** The first image
  ends at the offset `0x2850`, which is 10320 bytes. A second image with
  different link addresses starts at that offset. The vendor driver
  increases its transfer to a boundary of 64 bytes from a buffer with more
  than one image. This driver sends all 10368 bytes, because the tests used
  that form. The sensor uses only the first 10320 bytes.
- **The chip IDs `0x9338` and `0x9536`.** The original driver has an empty
  branch for them. This driver also does not do that step. The test unit
  gives `0x95a8`, thus no test uses this path. The Linux driver of FocalTech
  names a family of sensors (`ft9366`, `FT9338`, `FT9391`, `FT9361`,
  `FT9348`, `FT9536`, `FT9371`, `ft9362`) and never names `ft9201`. The
  community guides report that a driver for `0x9338` also operates the ID
  `0x93a9`. The sensor type comes from the OTP memory after the connection.
  The second firmware image in both vendor binaries is probably for one of
  those sensors.
- **The correct firmware revision.** Two revisions are known, and both
  operate. They differ in the geometry that they set (96x96 or 64x80). A
  test read the type of this sensor, and the type is 3. A device of the
  same type in a different project also captures 96 x 96. Thus the
  installed 96 x 96 image is correct for this unit. A revision with a
  larger geometry can exist, and it would be the most useful discovery for
  this sensor.
- **The header `dd dd`.** It is a constant marker, and the driver uses it as
  a check. The reason for those two bytes is not known, and no test shows a
  different valid value.
- **The retry counts.** The automatic power step succeeded at the first
  attempt in each test, thus no test used the retries.


## What other projects found

Other people work on this sensor. Their measurements come from different
hardware revisions, thus a fact from one of them can be incorrect for
another. These are the facts that are useful here.

**The sensor gives its own type.** `OMGrant/ft9201-libfprint` reads the
type before the firmware upload. The driver already sends that sequence,
and it discards the answer:

```
0x57                                 MCU download mode
0x66 wValue=0x00df wIndex=0x00c8     write
0x65 wIndex=0x00c8                   read, discard
0x66 wValue=0x001d wIndex=0x00f1     write
0x65 wIndex=0x00f4                   read -> b
0x66 wValue=(b | 1) wIndex=0x00f4    write
0x65 wIndex=0x00f3                   read -> raw
0x66 wValue=0x0000 wIndex=0x00f4     finish
type = (raw >> 1) & 0x0f
```

**A test on this hardware read those two locations.** The driver now
decodes the answers that it already read, and it puts them in the log:

- `0x00f3` reads `0x26`, thus the type of this sensor is **3**.
- `0x00f4` reads `0x00`. The driver writes the constant `0x0001` to that
  location, and the vendor writes the value that it read with the bit 0
  set. Those two operations give the same result here. The driver writes a
  warning if a different unit reads a value that is not `0x00`.

**The type does not show that the firmware image is incorrect.** The 64 x
80 is not a geometry of the sensor. It is the input size that the matcher
of the vendor wants. The device of that project is also the type 3, and
it also captures 96 x 96. It cuts the frame to 64 x 80 only before the call
to the matcher. This sensor reports 96 x 96 with the installed image, thus
that image is correct.

**The two bytes before the image can be a quality value.** That project
reads them as a quality value of the chip, and it discards a frame only
when both bytes are zero. `Cyd0n1a` records them as `00 00`. This driver
accepts only `dd dd`. If they are a quality value, this driver discards
frames that it can use.

**The chip ID `0x95a8` is the variant 3, the FT9361.** `Cyd0n1a` gives this
table:

| chip ID | variant | sensor |
|---|---|---|
| `0x9338` | 1 | FT9338W |
| `0x95a8` | 3 | FT9361 |
| `0x9536` | 6 | FT9536W |

**A different revision uses the request `0x6F`.** `NBN-PATRIC` measured a
64 x 80 unit where `0x35` arms nothing. On that unit the bulk read comes
from `0x6F`, with the length in `wValue` and the address in `wIndex`. That
path returns exactly width x height bytes, and it gives no header. A test
measured the `0x35` path of this driver on this hardware, and that path
operates. Thus the two revisions differ.

**The vendor renews the mode approximately each second.** That project
measured 1 finger detection in 4445 polls with no renewal. It then measured
24 detections with a renewal after each 30 polls of 30 ms. This driver
renews after 1000 polls of 80 ms, which is 80 seconds.

**GObject permits an instance of 64 KB.** A driver that holds the enrolment
frames in its instance structure stops with the message
`g_type_register_static_simple: assertion 'instance_size <= G_MAXUINT16'
failed`, and that message does not name the cause. This driver keeps the frames in a `GPtrArray` of `GBytes`, thus the limit
does not apply to it. Keep it that
way.

**The MCU configuration after the upload.** `OMGrant` takes it from the
Linux driver of FocalTech, where the functions have the names
`InitMcuConfig` and `SwitchNextSensorWorkMode`. The registers are `0x01`,
`0x41`, `0x30`, `0x22`, `0x23`, `0x54`, `0x1f` and `0x1e`. This driver
writes all of them except **`0x54` = `0x01`**.

## The MCU firmware: the other extraction methods

**The driver cannot communicate with this sensor before you install the
firmware.** The FT9201 keeps no firmware. Its 8051 core starts with an empty
code RAM after each loss of VBUS. Before the host sends an image to the bulk
OUT endpoint `0x02`, each AFE register read gives `00 00 00 00`.

The image is the property of the vendor. Therefore this project does not
supply it, and you cannot distribute it. You must take it from a vendor
binary. There are four methods. The extractor accepts each of them and
identifies the type itself.

The README gives the method to use. These three methods read a local file.

**Method 2, from the Windows driver binary.** The image is in the file
`ftUsbWbioDriver.dll`. You need only that one file from a Windows
installation. You do not need a capture:

```
C:\Windows\System32\drivers\UMDF\ftUsbWbioDriver.dll
```

A second copy is in the directory `C:\Windows\System32\DriverStore\FileRepos
itory\ftusbwbiodriver.inf_amd64_*\`.

```bash
./ft9201-extract-firmware.py ftUsbWbioDriver.dll -o ft9201.bin
sudo install -D -m 0644 ft9201.bin /lib/firmware/focaltech/ft9201.bin
```

The tests used the driver with `DriverVer` 11/29/2019, version 1.0.3.58, and
files from March 2023. That DLL holds **two** 8051 images, one after the
other, at the offsets `0x16690` and `0x18ee0`. The sensor gets the first
image. It has 10320 bytes, and the vendor sends 10368 bytes. The vendor
increases the length to a whole number of units of 64 bytes, thus it sends
48 bytes of the second image as filler. The extractor makes the same bytes,
and does not add zeros, because the tests confirmed that form. The second
image differs only in its link addresses. It is probably for one of the
other AFE types (`0x9338` or `0x9536`).

**Method 3, from the Linux driver of FocalTech.** You do not need a Windows
machine for this method. Their proprietary driver is a complete libfprint
build with their driver in it. It is available to the public, and it holds a
firmware image:

```bash
curl -LO https://github.com/ryenyuku/libfprint-ft9201/releases/download/1.94.4_20250219/libfprint-2-2_1.94.4+tod1-0ubuntu1.22.04.2_amd64_20250219.deb
ar x libfprint-2-2_*.deb && mkdir -p x && tar --zstd -xf data.tar.zst -C x
./ft9201-extract-firmware.py \
    x/usr/lib/x86_64-linux-gnu/libfprint-2.so.2.0.0 -o ft9201.bin
```

This gives a **different revision** with 10784 bytes and the SHA-256
`907cb638...`. The tests confirmed that this image starts the sensor. But it
sets the sensor to **64x80** and not to 96x96, thus the image is smaller.
The image size is the one condition that helps this sensor. Therefore use
the copy from the Windows driver if you can get it. The extractor identifies
both revisions and tells you which one you have.

**Method 4, from a USB capture.** Use this method when you have no binary.
It also shows the image that one machine sends:

```bash
./ft9201-extract-firmware.py capture.pcapng -o ft9201.bin
```

The extractor reads pcapng and USBPcap itself. It needs no `tshark` and no
other Python module. Each method gives the SHA-256 of the image, and tells
you if it agrees with the image that this project confirmed (`0999f2f4...`,
10368 bytes). A capture of more than one connection holds more than one
copy, and the extractor makes sure that the copies agree. **Each method
gives the same image**, which is also a good check of the extraction.

Do not give the file `ftWbioEngineAdapter.dll` to the extractor. That file
is the matcher and holds no firmware. The extractor tells you this.

If the file is not present, the activation fails with a message that gives
the path, and `journalctl -u fprintd` shows the message. The driver also
examines the length of the file. It refuses a file with an incorrect length,
or a length that is not a multiple of 64 bytes. It does not send unknown
data to the code RAM of the sensor.


## Faults found by a comparison with upstream drivers

A comparison of this driver with `elan.c`, `vcom5s.c`, `upektc_img.c` and
`vfs0090.c` showed three faults:

- **No `GCancellable`.** Each USB transfer gave `NULL` where the other
  drivers give `fpi_device_get_cancellable (dev)`. Thus a cancel operation
  during an enrol or a verify could not stop a transfer. The transfer
  continued to its timeout of 2 seconds. The driver now gives the true
  cancellable at each transfer.
- **A race condition between the activate and deactivate operations.** The
  `dev_deactivate()` function tested only a flag, and the driver set that
  flag after the activation state machine was complete. Thus a deactivate
  request during the activation saw an idle device, and the driver reported
  that the device was idle. But the activation state machine continued its
  USB transfers, and then started the capture loop. The driver now sets the
  busy flag for the full time of the activation and the capture. A
  deactivate request during the activation now has an effect at the end of
  the activation.
- **No test of the captured image.** A frame with one value in all pixels
  holds no ridge data. A sensor with a fault gives such a frame. The driver
  gave the frame to the matcher, and the matcher used it. The driver now
  discards such a frame and reports a capture error.

Each of these three faults is visible in the code. None of them needs the
hardware.


## Faults that only the hardware showed

- **The capture loop did not use the state machine of libfprint.** The
  driver had no `change_state` function, and it started one continuous
  capture loop from the activation. Thus it gave an image and immediately
  captured again, while the finger was still on the sensor. It called
  `fpi_image_device_image_captured()` after libfprint changed the device to
  `AWAIT_FINGER_OFF`. Each additional frame caused an assertion in
  libfprint, and then the bulk endpoint stopped. The enrolment failed after
  two stages. The driver now has the shape of the other image drivers, for
  example `nb1010.c`: one capture starts from `change_state` when libfprint
  asks for a finger.
- **A wait without a limit for the capture-ready state.** When the register
  `0x30` never gives `0xbb`, the loop polled for all time, and `fprintd-
  enroll` gave no message. The loop now has a limit of approximately five
  seconds. It then fails with a message that gives the register and the
  value.
- **The driver did not set `bz3_threshold`**, thus libfprint used its
  default of 40. A change to 24 had no effect, because no bozorth3 threshold
  operates on this sensor. The driver no longer uses that matcher. Refer to
  [`MATCHING.md`](MATCHING.md).
- **The firmware download was not in the driver.** This gave the sensor the
  appearance of a permanent fault. The next section gives the details.
- **The meson patch did not apply**, and eight calls of
  `fpi_ssm_*_delayed()` had an additional `GCancellable *` argument. The API
  removed that argument some years ago. Both faults come from a driver that
  was written for libfprint 1.90.


## The "needs a Windows reset" fault, and its cause

The README of the original kernel driver says: *"If something happens during
initialization and driver stops sending images, you need to plug it into a
windows machine which will reset it into a stable state."*

**There is no bad state.** The FT9201 keeps no firmware. After each loss of
VBUS its 8051 core has an empty code RAM, and each AFE register read gives
`00 00 00 00`. The host must send an image to the bulk OUT endpoint `0x02`.
The Windows vendor driver does this after each cold connection. The Linux
drivers never did it. Thus they operated only on a sensor that Windows had
prepared, and that had not lost its power after that. The step "plug it into
a windows machine" was not a reset. It was the firmware upload.

This also explains two other observations. No action on the host helped,
because no host action can put firmware into the device. And a test that
removed the power for 30 seconds made the condition worse, because it
removed the firmware.

### How the tests confirmed this

The tests used a USBPcap capture of the Windows driver during a full
enrolment. It has 155 seconds and 7 enumerations. That capture is not in
this repository: it holds the proprietary firmware image and fingerprint
data. It shows each part:

- Four cold connections each send the same 10368-byte image through the
  endpoint `0x02`. The data is 8051 code: `02` (LJMP) at the reset vector,
  `12` (LCALL), and `c2` and `d2` (bit clear and set) through the full
  image.
- Three warm enumerations read the register `0x20`, see `01 01`, and do no
  download. This driver uses the same test.
- Immediately after the download, the register `0x20` changes from `00 00`
  to `a5 5a`. The registers `0x14` and `0x15` then give 96x96, and the
  registers `0x16` and `0x17` give the chip ID `0x95a8`. Before the
  download, none of those registers gave an answer.

A test then sent the same sequence to the Linux unit with the "permanent
fault". The unit operated immediately and captured fingerprints. The driver
now does this sequence itself.

### One condition that has the appearance of the old fault

Zeroes in each register are **not** proof that the firmware is gone. The AFE
removes its own power when no step arms it again, and it then gives zeroes
for each read. A measurement checked the USB error codes, thus these are
true answers and not failed transfers. A finger starts the AFE for
approximately half a second. It then gives `0x43` = `01`, `0x1d` = `a0` and `0x20` = `a5 5a`. The AFE then gives zeroes again.

Therefore the driver sends the `0x22` wake handshake and reads again before
it makes a decision. The vendor driver does the same. Without this step, an
idle sensor gets an unnecessary firmware download at each activation. The
download does no damage, but it takes time.


## The finger detection stall

### The recovery that the driver now makes

The activation reads the register `0x20` after it arms the sensor. The
value `01 01` shows that the MCU searches for a finger. When the value is
different, the driver arms the sensor again, three times.

If the search still does not start, the driver **sends the firmware
again** and arms the sensor again. The sensor answers each register in this
condition, thus it has its firmware, but its search does not start. Before
this step the only correct operation was a power cycle of the USB port, and
a user cannot always do that.

**What a test confirmed:** the tool `tools/ft9201-fwload2` sent the image to
a sensor that already had it. The download gave the MCU status `a5 5a` and
the correct dimensions, and the sensor then captured two frames with the
driver. Thus the operation is safe on a sensor that operates.

**What a test did not confirm:** that the download starts the search on a
sensor in the stalled condition. That condition is not deterministic, thus a
test on demand is not possible. The recovery runs one time in each
activation, and the driver gives the earlier warning if the search still
does not start.

This section holds the full investigation of the fault that the README names as a known issue.

- **The finger detection stopped after one idle period. The driver now
  corrects this.** Each test below started from a port power cycle. Presses
  one after the other in one session gave **4 detections of 4**. But a frame
  and then an idle period of approximately 80 seconds left the next press
  without a detection. This occurred two times, and the renewal of the
  automatic sensor mode operated on time in both tests. In those runs **each
  of 2300 polls gave `00` with the USB status 0**. Thus the device answers,
  and it reports no finger.

  A usbmon capture compared this driver with the vendor driver. Both send
  the same requests, with the same values, at the same cadence of 81 ms. The
  activation, the frame, the arm operation after the frame and the renewal
  all agreed. The sensor also answered this driver correctly. But the sensor
  of the vendor driver keeps its detection through an idle period of 400
  seconds in one session.

  More tests isolated the cause. Presses with frames and no idle period gave
  4 of 4. Presses with **no frame** and then an idle period of 95 seconds
  gave detections two times. A frame and then the same idle period gave no
  detection, two times. Thus **a frame and then a long idle period** stops
  the detection. Presses alone do not, and an idle period alone does not.

  This showed the one step that the driver did not copy. The vendor driver
  does **three reads after each arm operation**: `0x20`, `0x1d` and `0x20`.
  This driver did none of them after a frame. The read of `0x1d` probably
  clears the finger latch. The last read of `0x20` is their check that the
  MCU looks for a finger again (`01 01`, the busy state). The driver now
  does all three reads after each arm operation. It also does the full arm
  operation again, up to `FT9201_REARM_MAX_RETRY` times, when the MCU does
  not continue.

  With that change the test passed **4 times of 4**. The test is a frame, an
  idle period and a press. Before the change it failed 2 times of 2. Each
  run started from its own power cycle, and each renewal came after exactly
  1000 idle polls. The retry never operated, thus the reads gave the
  improvement. A control run disabled the reads again, and changed nothing
  else. It failed in the old form. The driver took the frame, the renewal
  came on time, and the next press gave no report in 1375 polls. The result
  is **4 successes with the reads and 3 failures without them**, on the same
  unit in one evening.

  Two tools help with more tests, and they need no finger press. The tool
  `tools/ft9201-reg-watch` answers the question "is the sensor still
  armed?". On an armed sensor each register gives `01 01`, because the MCU
  is busy with the search for a finger. A stalled sensor gives `a5 5a` in
  the register `0x20`. The tool `tools/ft9201-idle-hunt` repeats the full
  cycle outside libfprint. It reports the condition where the register
  `0x1d` sees a finger and the request `0x43` does not.
- **The vendor driver renews the automatic sensor mode after each 1000 idle
  polls**, which is approximately 80 seconds. This driver now does the same.
  An earlier capture of 26 seconds is shorter than one renewal interval,
  thus it contains no renewal. That capture gave the incorrect conclusion
  that the vendor driver does not renew the mode. A capture of 14 minutes
  settles the question. Between one renewal and the next there are **exactly
  1001 polls of the request `0x43`**, four times in two device sessions. The
  measured interval moves between 79.2 s and 80.7 s, thus the vendor counts
  polls and not milliseconds. The renewal has three parts. First, the `0x22`
  wake handshake two times, with 16 ms between them. Then a delay of 48 ms.
  Then the value `0x01` in the registers `0x1f` and `0x1e`. This is the same
  sequence as after a frame, with the handshake before it. In that capture
  the sensor kept its detection through idle periods of 406 s and 397 s. It
  detected all four presses at the first poll after the press. The capture
  has 10 081 polls, 4 answers of `01`, and no failed transfer. An earlier
  test renewed the mode approximately each second. That is a different rate,
  and the tests rejected it.
- **The detection does not depend on the register sequence alone.** The tool
  `tools/ft9201-kill-bisect` starts from a sensor with a new power cycle. It
  applies the arm writes one at a time, and tests the detection after each
  write. One run gave a clear result. There was no detection through the
  first six writes. A detection started immediately after the write `0x1e =
  0x01`, with 25 detections in 50 polls. The detection continued through
  more wake and arm cycles. A second run with the same steps gave **no
  detection at each step**, and each register read `01 01`. Thus a variable
  exists that no register shows. Tests with one variable cannot give an
  answer until somebody finds it. This is the reason why earlier versions of
  this file changed their conclusions.
- **Conditions that the tests exclude.** Each of them has a measurement, and
  not an argument. The shape of the polls: the control traffic of this
  driver is equal to the traffic of the vendor driver. It has the same
  `bmRequestType 0xc0`, the same `wLength 1` and the same cadence of 81 ms.
  A usbmon capture confirms this. The tests also exclude these conditions:

    - a write or no write of `0x30 = 0xbb`
    - a test of `0x30` before a capture
    - the `0x22` wake handshake
    - a read of `0x20`, `0x30` or `0x1d` after an arm operation
    - a reset of the bulk engine
    - a USB device reset
    - USB autosuspend alone
- **What the vendor driver does with the register `0x30`.** It writes `0xbb`
  one time in each cold session, immediately after the firmware download.
  After that it only reads the register. The detection then operates for the
  full session, with 18 detections in 26 seconds. Thus the value `0xbb` and
  a correct detection can exist together, and this driver keeps the write.
  One test removed the write, and the detection came back. But the match
  operation then failed. The correctly enrolled finger gave 0.029 to 0.032,
  which is the noise level of a different finger. With the write it gives
  0.136 to 0.388. The write arms the imaging path. Its other effects are not
  known.
- **USB autosuspend makes the condition worse.** The hwdb of Fedora starts
  autosuspend for `2808:93a9`. A test measured 105 s in the suspend state
  against 36 s in the active state. Set `power/control` to `on` for this
  device. The directory `packaging/` has a udev rule for this.
- **The stall has the same effect on each action.** An earlier note here
  said that the `capture` action never detects a finger and the `enroll`
  action always detects it. That note was incorrect, and it came from the
  order of the tests. A new test in the stalled state showed that `enroll`
  also fails.
