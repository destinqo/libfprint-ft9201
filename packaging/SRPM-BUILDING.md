# Building the SRPM

This directory has everything `rpmbuild` needs:

- `libfprint-ft9201.spec`
- `0001-add-focaltech-ft9201-driver.patch`
- `focaltech_ft9201.c` / `focaltech_ft9201.h`

`Source0` in the spec points at Fedora 44's exact upstream release
(`libfprint v1.94.100`), so this package is a same-version drop-in
replacement for the stock runtime library, not an upgrade or downgrade.

> **Note on the version.** Fedora 44's repositories still carry
> `libfprint-1.94.10` alongside `1.94.100`. Those are *not* the same
> version — `1.94.100` is newer and is what is actually installed.
> Earlier revisions of this spec pinned `1.94.10`, which would have
> downgraded the system library.

## What was actually verified, and how

The driver now builds and runs on a real Fedora 44 machine against the
real `libfprint v1.94.100` source, with the physical sensor attached.
That build was a plain `meson setup` + `meson compile` (the same flags
the spec's `%meson`/`%meson_build` macros expand to), zero warnings, and
`fprint-list-supported-devices` correctly lists
`2808:93a9 | FocalTech FT9201 Fingerprint Sensor`.

Fixing that build turned up several packaging bugs that an earlier
sandbox-only check had missed:

- **The meson patch did not apply at all.** libfprint reorganised driver
  registration after 1.90: `default_drivers` is now derived from a
  `drivers_info` dictionary, `endian_independent_drivers` no longer
  exists, and `driver_sources` entries use `files()`. The patch has been
  rewritten for the current layout and verified to apply cleanly to
  `v1.94.100`.
- **Two `BuildRequires` were missing**: `openssl-devel` (needed by the
  non-optional `uru4000` driver) and `libgudev-devel` (needed by
  `elanspi`). meson calls `error()` and stops if either is absent.
- **`pkgconfig(json-glib-1.0)` was listed but is not referenced anywhere
  in libfprint.** Removed.
- **`%files` was incomplete**: the build also installs
  `70-libfprint-2.rules` and
  `metainfo/org.freedesktop.libfprint.metainfo.xml`. An installed but
  unpackaged file is a hard `rpmbuild` failure.
- **`-Dintrospection=false` cannot be used.** That option selects a
  libfprint code path with an upstream bug — `tests/meson.build:295`
  iterates the `drivers_tests` dictionary with a single loop variable,
  which meson 1.11 rejects outright ("Foreach expects exactly 2
  variables for iterating over objects of type dict"). Fedora never hits
  it because Fedora builds with introspection on. The spec now does the
  same, which also means it ships the `FPrint-2.0` typelib exactly like
  the stock package.

**What this still does not confirm:**
- The real `%meson`/`%meson_build`/`%meson_install` macros, and a full
  `rpmbuild -bb` / `mock` run. The build was reproduced with equivalent
  `meson`/`ninja` calls; the spec itself parses (`rpmspec -q` resolves
  it to `libfprint-ft9201-1.94.100-1.fc44.x86_64`) but has not been run
  through `mock`. Do that before trusting it — see below.
- The sensor operates from end to end. Tests on real hardware confirm the
  image capture, a complete enrolment and a verification. The README gives
  the measurements.

## Producing and checking the real SRPM (do this on Fedora, or in mock)

```bash
sudo dnf install rpmdevtools rpm-build
rpmdev-setuptree

cp libfprint-ft9201.spec ~/rpmbuild/SPECS/
cp 0001-add-focaltech-ft9201-driver.patch focaltech_ft9201.c focaltech_ft9201.h \
   ~/rpmbuild/SOURCES/

cd ~/rpmbuild/SPECS
spectool -g -R libfprint-ft9201.spec     # fetches Source0 from gitlab.freedesktop.org
rpmbuild -bs libfprint-ft9201.spec       # -> SRPMS/libfprint-ft9201-1.94.100-1.*.src.rpm
```

Then build and sanity-check it properly in a clean chroot (this catches
missing BuildRequires and anything else that only a real Fedora build
environment would catch):

```bash
sudo dnf install mock
sudo usermod -a -G mock "$USER"    # re-login (or `newgrp mock`) after this
mock -r fedora-44-x86_64 --rebuild ~/rpmbuild/SRPMS/libfprint-ft9201-1.94.100-1.*.src.rpm
```

`mock` output lands in `/var/lib/mock/fedora-44-x86_64/result/` —
that's your real, Fedora-built `libfprint-ft9201-*.rpm`.

```bash
rpmlint ~/rpmbuild/SRPMS/libfprint-ft9201-1.94.100-1.*.src.rpm
rpmlint /var/lib/mock/fedora-44-x86_64/result/libfprint-ft9201-*.rpm
```

## Installing it

```bash
sudo dnf install /var/lib/mock/fedora-44-x86_64/result/libfprint-ft9201-*.x86_64.rpm
sudo systemctl restart fprintd
fprintd-list "$USER"
```

`dnf` will offer to remove the stock `libfprint` package because of the
`Conflicts: libfprint` / `Provides: libfprint` pair in the spec — that's
expected, it's what makes this a clean swap instead of a file conflict.
`libfprint-devel` is untouched (see the spec's `%install` for why) so it
stays installed if you have it.

## Keeping it installed across updates

Same caveat as with the earlier manual `meson install` approach: a
`dnf update` that pulls in a newer `libfprint` build will try to put the
stock package back over this one. Lock it:

```bash
sudo dnf install python3-dnf-plugin-versionlock
sudo dnf versionlock add libfprint-ft9201
```

## Automatic "libfprint moved upstream" hook

Since `libfprint-ft9201` is versionlocked, `dnf` never actually transacts
it during a normal `dnf update` — which also means a dnf transaction
hook would never fire for it. Instead, `ft9201-update-check.sh` (with a
systemd timer) periodically asks the repo metadata directly whether
Fedora's real `libfprint` package has moved since last time, and — if
so — attempts a full rebuild automatically.

**It builds automatically; it does not install automatically (by
default).** Rebuilding is safe to automate: worst case it wastes some
CPU time. Installing a fingerprint-auth library unattended is a
different kind of risk (a broken or unexpectedly-conflicting build could
land on a running system with nobody watching), so that step stops and
logs install instructions instead, unless you explicitly opt in
(`FT9201_AUTO_INSTALL=yes`, commented out in the `.service` file).

What it does on each run:
1. Compares the current repo `libfprint` NEVR against the last one seen.
2. No change → logs and exits.
3. Change detected → regenerates the spec for the new version, fetches
   the new source tarball, and first tries just `%prep` (patch
   application) — cheap, and the step most likely to fail if Fedora
   backported a driver into the same `meson.build` region ours patches.
   If the patch doesn't apply: logs it and stops. Nothing is built.
4. Patch applied cleanly → runs the full build (installing missing
   `BuildRequires` via `dnf builddep` first if needed).
5. Build fails → logs it and stops.
6. Build succeeds → copies the RPM to
   `/var/lib/ft9201-update-check/rpms/` and logs the exact `dnf` command
   to review and install it — or, if `FT9201_AUTO_INSTALL=yes`, installs
   it, re-applies `versionlock`, and restarts `fprintd` automatically.

This was validated in this sandbox end to end with `dnf`, `spectool`,
`rpmbuild`, `systemctl`, and `logger` stubbed out, driving the script
through: first run, a clean successful rebuild, a patch-application
failure, a build failure, and the opt-in auto-install path. All five
stopped or proceeded exactly as intended. The real `dnf repoquery` /
`dnf builddep` / `spectool` calls against actual Fedora repos, and a
real `rpmbuild` run, were not (and cannot be) exercised outside a real
Fedora system.

```bash
# Keep the spec + patch + driver sources where the script expects them
sudo mkdir -p /usr/local/share/ft9201-srpm
sudo cp libfprint-ft9201.spec 0001-add-focaltech-ft9201-driver.patch \
        focaltech_ft9201.c focaltech_ft9201.h \
        /usr/local/share/ft9201-srpm/

sudo dnf install rpmdevtools rpm-build dnf-plugins-core

sudo install -Dm755 ft9201-update-check.sh /usr/local/sbin/ft9201-update-check.sh
sudo install -Dm644 ft9201-update-check.service /etc/systemd/system/ft9201-update-check.service
sudo install -Dm644 ft9201-update-check.timer /etc/systemd/system/ft9201-update-check.timer
sudo systemctl daemon-reload
sudo systemctl enable --now ft9201-update-check.timer

# Run it once by hand to confirm it actually works against your repos
# and to record the initial baseline version:
sudo systemctl start ft9201-update-check.service
journalctl -t ft9201-update-check -n 30
```

Check on it any time:

```bash
systemctl status ft9201-update-check.timer
journalctl -t ft9201-update-check
cat /var/lib/ft9201-update-check/last-seen-nevr
ls /var/lib/ft9201-update-check/rpms/          # anything built and waiting for you
```

Default schedule is weekly (`OnCalendar=weekly` in the `.timer` file,
`Persistent=true` so a laptop that was off catches up on the next boot).
Edit the `.timer` file and `daemon-reload` if you want it more or less
frequent.

To enable full auto-install (build **and** install, no confirmation),
uncomment the `Environment=FT9201_AUTO_INSTALL=yes` line in
`ft9201-update-check.service` and `daemon-reload`. Understand what
you're opting into first: a successfully-built package gets installed
and `fprintd` restarted with nobody reviewing it first.
