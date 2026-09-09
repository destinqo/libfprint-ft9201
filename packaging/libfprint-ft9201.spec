%global libfprint_version 1.94.100

Name:           libfprint-ft9201
Version:        %{libfprint_version}
Release:        5%{?dist}
Summary:        libfprint runtime rebuilt with an added FocalTech FT9201 (2808:93a9) driver

# Same license as the vendored libfprint sources it is built from.
License:        LGPL-2.1-or-later
URL:            https://github.com/banianitc/ft9201-fingerprint-driver
# Pinned to the exact libfprint release Fedora 44 ships, so this package
# is a drop-in replacement for the stock runtime library, not an upgrade
# or downgrade of it.
Source0:        https://gitlab.freedesktop.org/libfprint/libfprint/-/archive/v%{libfprint_version}/libfprint-v%{libfprint_version}.tar.gz
# The new driver itself (out-of-tree until/unless it is proposed upstream).
Source1:        focaltech_ft9201.c
Source2:        focaltech_ft9201.h
# Helper that extracts the required MCU firmware from a USB capture of the
# Windows vendor driver. The firmware itself is proprietary and is NOT
# shipped here; see the %%description.
Source3:        ft9201-extract-firmware.py
# Keeps the sensor out of USB autosuspend, which Fedora's hwdb otherwise
# enables for this device and which its finger detector does not survive.
Source4:        60-ft9201-no-autosuspend.rules
# Two-line change to meson.build and libfprint/meson.build that wires the
# new driver into the "drivers_info" and "driver_sources" dictionaries,
# the same way any other built-in libfprint driver is registered.
Patch0:         0001-add-focaltech-ft9201-driver.patch

BuildRequires:  meson >= 0.49.0
BuildRequires:  ninja-build
BuildRequires:  gcc
BuildRequires:  gcc-c++
BuildRequires:  pkgconfig(glib-2.0) >= 2.56
BuildRequires:  pkgconfig(gio-unix-2.0)
BuildRequires:  pkgconfig(gobject-2.0)
BuildRequires:  pkgconfig(gusb)
BuildRequires:  pkgconfig(pixman-1)
BuildRequires:  pkgconfig(nss)
BuildRequires:  pkgconfig(cairo)
BuildRequires:  pkgconfig(udev)
# uru4000 needs OpenSSL >= 3.0 and elanspi needs gudev. Both drivers are
# non-optional, and meson stops with an error() if either is missing.
BuildRequires:  openssl-devel
BuildRequires:  libgudev-devel
# Introspection cannot be turned off: see the %%build comment.
BuildRequires:  gobject-introspection-devel

# For the firmware extraction helper. It uses only the standard library.
Requires:       python3

# This package ships the same soname (libfprint-2.so.2) as Fedora's own
# "libfprint" package and is meant to replace it, not sit alongside it.
# It intentionally does NOT touch libfprint-devel (headers/.pc/unversioned
# .so are not shipped here — see %%install) so that stays installable and
# unmodified for anything that builds against libfprint.
Conflicts:      libfprint
Provides:       libfprint = %{version}-%{release}
Provides:       libfprint%{?_isa} = %{version}-%{release}

%description
This is Fedora's own libfprint %{libfprint_version} source, rebuilt with
one additional built-in image-device driver for the FocalTech FT9201
USB fingerprint reader (USB\VID_2808&PID_93A9), which has no upstream
libfprint support.

The USB protocol the new driver speaks was reverse-engineered from the
vendor Windows driver by the banianitc/ft9201-fingerprint-driver project
as an out-of-tree Linux kernel module, then corrected against a USB
capture of the Windows driver and verified on real hardware.

IMPORTANT — firmware is required and is not included. The FT9201 keeps
no persistent firmware: its MCU comes up with empty code RAM after every
loss of bus power, and the host has to download a vendor-proprietary
image before any part of the sensor answers. That image cannot be
redistributed, so it has to be extracted from your own vendor driver.
The image is embedded in the vendor's Windows driver binary, so copying
that one file off a Windows install is enough:

    ft9201-extract-firmware ftUsbWbioDriver.dll -o ft9201.bin
    sudo install -D -m 0644 ft9201.bin \
        /lib/firmware/focaltech/ft9201.bin

(the file lives at C:\Windows\System32\drivers\UMDF\). A USBPcap
capture of the Windows driver yields the identical image, and if no
Windows machine is available, FocalTech's own publicly downloadable
Linux driver carries a firmware image the extractor also accepts -- see
the driver README. That revision configures the sensor for 64x80 rather
than 96x96, so the Windows one is preferable.

Until that file exists, the driver reports the device but fails to
activate it, naming the missing path in the journal.

Verification is done by the driver itself, by correlating the ridge
pattern, and not through libfprint's minutiae matcher: a 96x96 frame from
this sensor yields only 1-2 minutiae while bozorth3 refuses to score
fewer than 10, so that route cannot work here at all. The correlation
matcher is open code, not extracted from any vendor binary.

Verification is measured working: the enrolled finger matched 4 of 4 and
a different finger was accepted 0 of 9, with the threshold sitting in a
gap that 23 impostor comparisons never came near. That is still one pair
of fingers on one person, so it is not a false-accept rate -- treat it as
demonstrably discriminating rather than measured secure, and re-check on
more fingers before relying on it alone.

Known issue: finger detection sometimes stalls between sessions, and
then nothing detects the finger -- not this driver, not a bare libusb
poller, and not any particular action, so it is a sensor state rather
than a driver bug. A USB port power cycle usually clears it. See the
driver README.

%prep
%autosetup -p1 -n libfprint-v%{libfprint_version}
cp -f %{SOURCE1} libfprint/drivers/
cp -f %{SOURCE2} libfprint/drivers/
cp -f %{SOURCE3} .

%build
# doc: gtk-doc is not needed for a runtime-only rebuild and pulls in a
#   heavier BuildRequires chain for no benefit here.
# introspection: left ON, and it must stay on. libfprint 1.94.100 has a
#   bug in the code path that -Dintrospection=false selects:
#   tests/meson.build:295 iterates the "drivers_tests" dictionary with a
#   single loop variable, which meson 1.11 rejects outright ("Foreach
#   expects exactly 2 variables for iterating over objects of type dict").
#   Fedora never hits it because Fedora builds with introspection on.
#   Keeping it on also means this package ships the FPrint-2.0 typelib,
#   exactly like the stock libfprint package, so the swap stays a true
#   drop-in replacement.
# installed-tests: off, as Fedora's own libfprint build has it. They are
#   driver test fixtures and their own binaries, none of which belong in a
#   runtime-only replacement package -- and an installed but unpackaged
#   file is a hard rpmbuild failure.
%meson -Ddoc=false -Dinstalled-tests=false
%meson_build

%install
%meson_install

# Ship only the runtime library + its udev hwdb data. Headers, the
# unversioned .so symlink, and the .pc file are deliberately left out so
# this package never conflicts with Fedora's own libfprint-devel.
rm -rf %{buildroot}%{_includedir}/libfprint-2
rm -f %{buildroot}%{_libdir}/libfprint-2.so
rm -f %{buildroot}%{_libdir}/pkgconfig/libfprint-2.pc
# The .gir belongs to Fedora's libfprint-devel, same as the headers; only
# the typelib is a runtime file.
rm -f %{buildroot}%{_datadir}/gir-1.0/FPrint-2.0.gir
rmdir --ignore-fail-on-non-empty %{buildroot}%{_datadir}/gir-1.0 2>/dev/null || :
rmdir --ignore-fail-on-non-empty %{buildroot}%{_libdir}/pkgconfig 2>/dev/null || :

# The firmware extraction helper. Deliberately a real command rather than
# a %%doc file, because every user of this package has to run it once.
install -D -m 0755 ft9201-extract-firmware.py \
    %{buildroot}%{_bindir}/ft9201-extract-firmware

# Keeps the sensor out of USB autosuspend. Fedora's hwdb enables it for
# 2808:93a9, and a suspended sensor answers nothing.
install -D -m 0644 %{SOURCE4} \
    %{buildroot}%{_udevrulesdir}/60-ft9201-no-autosuspend.rules

%post -p /sbin/ldconfig
%postun -p /sbin/ldconfig

%files
%license COPYING
%doc README.md NEWS
%{_libdir}/libfprint-2.so.2
%{_libdir}/libfprint-2.so.2.0.0
%{_udevhwdbdir}/60-autosuspend-libfprint-2.hwdb
%{_udevrulesdir}/70-libfprint-2.rules
%{_udevrulesdir}/60-ft9201-no-autosuspend.rules
%{_datadir}/metainfo/org.freedesktop.libfprint.metainfo.xml
%{_libdir}/girepository-1.0/FPrint-2.0.typelib
%{_bindir}/ft9201-extract-firmware

%changelog
* Wed Sep 09 2026 <you> - 1.94.100-5
- Enrolment now takes 15 frames and not 8. A measurement gave the larger
  value. It scored each saved frame against a template of the other
  frames, and the rate of the incorrect reject operations falls until 14
  frames. On the hardware the correct finger now scores 0.239 to 0.357.
  A different finger scores 0.024 to 0.027. Thus the distance between the
  two groups increases from 3.0 to 8.9 times. A template that an earlier
  version wrote holds 8 frames, and it stays valid.
- The variable FT9201_MATCH_THRESHOLD changes the match threshold at run
  time, for a measurement. The driver accepts only one number between
  0.01 and 1.00. It keeps the compiled default of 0.06 for each other
  content. It writes a warning for each accepted value.
- The driver decodes the sensor type that the firmware download already
  read from the SFR space, and it puts the type in the log. This unit is
  the type 3, and it reports 96 x 96. Thus the installed firmware image is
  correct for it.
- The documentation moves from one file of 787 lines to a README of 246
  lines, docs/PROTOCOL.md and docs/MATCHING.md.

* Mon Sep 07 2026 <you> - 1.94.100-2
- Driver now downloads the sensor's MCU firmware, which is what the
  "needs a Windows machine to recover" bug always was: the FT9201 keeps
  no persistent firmware and no host-side reset can substitute for the
  download. Removed the %%description paragraph claiming the fault was
  unrecoverable.
- Ship ft9201-extract-firmware, since the firmware image is proprietary
  and cannot be packaged. Added the python3 dependency it needs. It takes
  either the vendor Windows driver binary, which carries the image
  embedded, or a USB capture; both were verified to yield the same image.
- Verification now works: the driver is a plain FpDevice matching with its
  own keypoint/RANSAC comparison rather than libfprint's bozorth3, which
  cannot score a 96x96 frame at all. Measured 4 of 4 genuine matches and 0
  of 9 impostor accepts.
- Ship a udev rule keeping the sensor out of USB autosuspend; Fedora's hwdb
  enables it for this device and the finger detector does not survive it.

* Mon Sep 07 2026 <you> - 1.94.100-1
- Initial package: libfprint 1.94.100 + focaltech_ft9201 driver
- Pinned to 1.94.100, the version Fedora 44 ships. 1.94.10 is also still
  in the repositories, but it is older and installing it would be a
  downgrade of the stock runtime library.
- Rewrote the meson patch for the driver registration layout introduced
  after 1.90: "default_drivers" is now computed from a "drivers_info"
  dictionary, "endian_independent_drivers" no longer exists, and
  "driver_sources" entries use files().
- Added the openssl-devel and libgudev-devel BuildRequires, dropped the
  unused json-glib one, and packaged the udev rules and metainfo files
  the build installs.
- Stopped passing -Dintrospection=false. That option selects a libfprint
  code path that meson 1.11 refuses to configure, and leaving
  introspection on restores the FPrint-2.0 typelib that the stock
  package ships.
