# FocalTech FT9201 (2808:93a9) driver for libfprint

An open source `libfprint` driver for the FocalTech FT9201 USB fingerprint
reader (`USB\VID_2808&PID_93A9`). The sensor operates through the usual
`fprintd` and PAM stack. You do not need an out-of-tree kernel module, a
patched proprietary `.so`, or a Secure Boot MOK enrolment.

> **This driver is experimental. Keep your password authentication.**
> A measurement used 13 fingers of 5 persons. It gives 0 incorrect accept
> operations in 1536 comparisons. Thus that rate is below approximately
> 0.2 % at 95 % confidence, and the equal error rate is 4.7 %. All 5 persons live in one house, thus they are
> not a sample of a population. Approximately one press of nine scores
> below the threshold and needs a second press. Do not use this sensor as
> the only authentication factor. The file
> [`docs/MATCHING.md`](docs/MATCHING.md) gives the measurements and their
> limits, and the section "Known issues" gives the faults that remain.


## What this is

- `driver/focaltech_ft9201.c` and `.h`: a driver that uses the driver API of
  libfprint. It is a plain `FpDevice`, and it does its own match operation.
  [`docs/MATCHING.md`](docs/MATCHING.md) gives the reason.
- The USB protocol comes from two sources. It started as the protocol of the
  out-of-tree kernel module of
  [banianitc](https://github.com/banianitc/ft9201-fingerprint-driver).
  This driver moves that protocol to the asynchronous state machine
  (`FpiSsm`) of libfprint, and does not use a kernel `read()` loop. A
  USBPcap capture of the Windows vendor driver during a complete enrolment
  then confirmed each step. Where the two sources disagreed, the capture
  won. The largest correction is the MCU firmware download. The kernel
  driver has no such download, and its absence is the full explanation of
  the "needs a Windows machine" fault.
- `./ft9201-extract-firmware.py`: gets the necessary firmware image. With
  `--download` it takes the vendor driver from the public Microsoft Update
  Catalog, thus you need no Windows machine. It also accepts a local vendor
  binary or a USB capture. Refer to "The MCU firmware". **The driver does
  not operate without that image.**
- `packaging/0001-add-focaltech-ft9201-driver.patch`: the small change to
  the `meson.build` files of libfprint that adds this driver to the build.
  The RPM uses the same file.


## What the tests confirm

The driver builds with no warnings against libfprint **v1.94.100**, which is
the version in Fedora 44. It also passes the code style check of libfprint
(`scripts/uncrustify.sh`). The command `fprint-list-supported-devices` gives
this line:

```
2808:93a9 | FocalTech FT9201 Fingerprint Sensor
```


The tests used a physical `2808:93a9` unit. A complete enrolment passes, a
cold start with no firmware passes, and the sensor operates for the login,
`sudo` and the lock screen. [`docs/PROTOCOL.md`](docs/PROTOCOL.md) gives
the full list of what the tests confirmed, what they did not confirm, and
the questions that stay open.

## How to build on Fedora 44

```bash
sudo dnf install meson ninja-build gcc git \
    glib2-devel libgusb-devel nss-devel pixman-devel \
    gobject-introspection-devel gtk-doc

# Use the version that Fedora 44 supplies. The patch is for the driver
# registration of v1.94.100. libfprint changed that layout after 1.90:
# it now makes default_drivers from a drivers_info dictionary. Thus the
# patch does not apply to all versions.
git clone --depth 1 -b v1.94.100 \
    https://gitlab.freedesktop.org/libfprint/libfprint.git
cd libfprint

# Copy the driver into the tree and apply the build patch.
cp /path/to/driver/focaltech_ft9201.c libfprint/drivers/
cp /path/to/driver/focaltech_ft9201.h libfprint/drivers/
patch -p1 < /path/to/packaging/0001-add-focaltech-ft9201-driver.patch

# The option --prefix=/usr is necessary, because fprintd looks in /usr
# and not in /usr/local.
meson setup build --prefix=/usr -Ddoc=false
meson compile -C build
sudo meson install -C build
sudo systemctl restart fprintd
```

Use the option `-Ddoc=false` only when `gtk-doc` is not installed.

For Fedora there is also an RPM. The directory `packaging/` holds
`libfprint-ft9201.spec`. That package replaces the `libfprint` package of
Fedora, and it keeps the driver through a system update better than a `meson
install`. Refer to "How to keep the driver after an update".


## The MCU firmware

**The driver cannot communicate with this sensor before you install the
firmware.** The FT9201 keeps no firmware. Its 8051 core starts with an empty
code RAM after each loss of VBUS. Before the host sends an image to the bulk
OUT endpoint `0x02`, each AFE register read gives `00 00 00 00`.

The image is the property of the vendor. Therefore this project does not
supply it, and you cannot distribute it. You must take it from a vendor
binary. There are four methods. The extractor accepts each of them and
identifies the type itself.

**Method 1, and the one to use: let the extractor get the driver.** You
need no Windows machine, no account and no vendor file:

```bash
./ft9201-extract-firmware.py --download -o ft9201.bin
sudo install -D -m 0644 ft9201.bin /lib/firmware/focaltech/ft9201.bin
```

Microsoft publishes the same vendor driver in their Update Catalog, which
is public. The extractor searches that catalog for the hardware ID
`USB\VID_2808&PID_93A9`, takes the cabinet file, and reads the firmware
from the driver in it. The result is the image `0999f2f4...`, which is the
image that the tests on the hardware used. The three methods below give
the same result from a local file.

This method needs a tool that opens a Microsoft cabinet:

```bash
sudo dnf install cabextract      # Fedora
sudo apt install cabextract      # Debian and Ubuntu
```

The extractor also accepts `p7zip` or `bsdtar` for this step. It tells you
when the system has none of them.

[`docs/PROTOCOL.md`](docs/PROTOCOL.md) gives three more methods that read
a local vendor file, and it gives the provenance of the image.

## How to enrol and verify

```bash
fprintd-list "$USER"           # gives "FocalTech FT9201 Fingerprint Sensor"
fprintd-enroll                 # 15 presses of the same finger
fprintd-verify                 # gives a match for the enrolled finger
```

**Press the same part of the finger each time during the enrolment.** Do not
move the finger between the presses. This is the opposite of the usual
advice for a large sensor, and a measurement gives the reason. The image
area is only 4.5 mm, and a verify operation compares one press against the
template. A template with 15 different parts of the finger gives one
press only one frame for the comparison.
[`docs/MATCHING.md`](docs/MATCHING.md) gives the numbers.

The examples of libfprint need no daemon and write full debug data. They are
faster for a test:

```bash
sudo ./build/examples/img-capture /tmp/finger.pgm
sudo ./build/examples/enroll
```

libfprint has a thermal model. After a few minutes of continuous operation
it stops the action with the message "Device disabled to prevent
overheating". A wait for a finger is also active time. This is a default of
the library, it applies to each driver, and this driver does not change it.
Thus a test must present a finger quickly.


## How to use it for the login, sudo and lock screen

> **Test the verification first, and keep the password.** The driver has
> its own matcher, and its threshold has data from one person only. Refer
> to "Verification: how it operates". Enrol a finger, verify the correct
> finger, verify a different finger, and only then make the change below.
>
> The change below adds the fingerprint as one more method. It does not
> remove the password, and you must not remove it. A fingerprint that the
> sensor does not accept then leaves you with a method that operates.

Fedora uses `authselect` and not `pam-auth-update`:

```bash
sudo authselect current
sudo authselect enable-feature with-fingerprint
sudo authselect apply-changes
```

GDM and the lock screen of GNOME use fprintd automatically. You do not need
a workaround with xsecurelock. That workaround was necessary only for the
"Fly" desktop of Astra Linux in the earlier guide for the proprietary
driver.


## How to keep the driver after an update

A `meson install` writes over the file `/usr/lib64/libfprint-2.so.2` of
Fedora. A `dnf update` of the `libfprint` package then puts the standard
library back, and gives no message. There are three solutions.

Use the RPM from `packaging/`. That package replaces `libfprint`. Then
`dnf` reports a conflict, and it does not replace the library without a
message:

```bash
sudo dnf swap libfprint ./libfprint-ft9201-*.rpm
```

Or lock the version:

```bash
sudo dnf install python3-dnf-plugin-versionlock
sudo dnf versionlock add libfprint
```

Or do the `sudo meson install -C build` again after each update of
`libfprint`. The command `rpm -V libfprint` then reports that the library is
different. This is correct and has no bad effect. It only shows that rpm
sees the change.


## Known issues

- **The finger detection can stop after an idle period.** The driver renews
  the automatic sensor mode, and that corrects the fault in each test. The
  sensor still stops in rare cases. The driver then sends the firmware
  again, which is a recovery that needs no power cycle. A power cycle of
  the USB port stays the last operation that always works.
  [`docs/PROTOCOL.md`](docs/PROTOCOL.md) holds the full investigation, the
  conditions that the tests exclude, and the measurements.
- **Turn off the USB autosuspend for this device.** The hwdb of Fedora
  starts the autosuspend after 2 seconds, and it makes the fault worse. The
  file `packaging/60-ft9201-fingerprint.rules` does this.
- **Enrol again after you move the sensor.** The driver handles a turn of
  the finger, but a new position of the sensor makes the person put the
  finger down at a new angle. The sensor then sees a different area of the
  skin. A test gave no match 3 times of 3 with the old template, and a
  match 4 times of 4 after a new enrolment.
- **A weak press gives a score below the threshold**, and it needs a second
  press. A measurement of 128 presses gives 10.9 % at the threshold 0.08. A
  press that is not on the centre of the sensor is the largest cause.
  [`docs/MATCHING.md`](docs/MATCHING.md) gives the numbers.

## Where the details are

- [`docs/PROTOCOL.md`](docs/PROTOCOL.md) — the USB protocol, the registers,
  the firmware download, what the tests confirmed, the open questions, and
  what other projects found.
- [`docs/MATCHING.md`](docs/MATCHING.md) — how the verification operates,
  the measurements and their limits, how to measure it yourself, and the
  methods that the tests rejected.
- [`tools/README.md`](tools/README.md) — the diagnostic programs.
- [`CONTRIBUTING.md`](CONTRIBUTING.md) — how to send a change.

## License

LGPL 2.1 or later, which is the license of libfprint. Refer to the file
headers. This license is necessary for code in the libfprint tree.
