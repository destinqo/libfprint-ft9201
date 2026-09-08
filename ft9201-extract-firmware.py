#!/usr/bin/env python3
r"""Extract the FT9201 MCU firmware image from the vendor's Windows driver.

The FocalTech FT9201 keeps no persistent firmware: the host has to download
an 8051 image over bulk-OUT endpoint 0x02 after every loss of VBUS. The
image is vendor-proprietary and cannot be redistributed, so it has to be
taken from the vendor's own driver. Two sources work, and this script
accepts either and tells them apart by itself:

  ftUsbWbioDriver.dll   The vendor's Windows UMDF driver, which carries the
                        image embedded in it. This is the easy way: no
                        capture and no Wireshark needed, just the file. On
                        a Windows install it lives at
                        C:\Windows\System32\drivers\UMDF\, and a copy
                        sits in
                        C:\Windows\System32\DriverStore\FileRepository\
                        ftusbwbiodriver.inf_amd64_*\

  libfprint-2.so.2.0.0  FocalTech's proprietary Linux driver, which is a
                        whole libfprint build with their driver compiled
                        in, and which carries a firmware image too. It is
                        publicly downloadable, so this route needs no
                        Windows machine at all. Note that the revision
                        found there configures the sensor for 64x80 rather
                        than 96x96 -- a smaller image, so prefer the
                        Windows driver's copy when you have it.

  capture.pcapng        A USBPcap capture of the Windows driver performing
                        the download. Useful when no binary is to hand, or
                        to confirm what a given machine really uploads.
                        Making one is described in the driver README.

There is also a fourth route, and it needs no vendor file at all. Microsoft
publishes the same Windows driver in their Update Catalog, which is public
and needs no account. The option --download takes it from there, and gives
the same image. Refer to the function download_driver_cab().

The Windows driver and a capture of it were verified to yield a
byte-identical image. The Linux library holds a different revision, which
was also confirmed to boot the sensor.

Everything here is standard library: pcapng, USBPcap and the byte search
through the driver binary are all parsed in this file.

Usage:
    ./ft9201-extract-firmware.py --download           -o ft9201.bin
    ./ft9201-extract-firmware.py ftUsbWbioDriver.dll  -o ft9201.bin
    ./ft9201-extract-firmware.py capture.pcapng       -o ft9201.bin
    sudo install -D -m 0644 ft9201.bin /lib/firmware/focaltech/ft9201.bin
"""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import struct
import sys
import tempfile
import urllib.parse
import urllib.request
from pathlib import Path

# pcapng block types we care about.
BLOCK_SHB = 0x0A0D0D0A
BLOCK_IDB = 0x00000001
BLOCK_EPB = 0x00000006
BLOCK_SPB = 0x00000003

SHB_BYTE_ORDER_MAGIC = 0x1A2B3C4D
LINKTYPE_USBPCAP = 249

FT9201_BULK_OUT_EP = 0x02
CHUNK_SIZE = 64

# Bounds on what we are willing to call a firmware image. The known-good
# image is 10368 bytes; these are wide enough to allow for a different
# vendor driver revision but narrow enough to reject nonsense.
MIN_FW_SIZE = 1024
MAX_FW_SIZE = 64 * 1024

# The image that this project verified on real hardware, for reference only.
KNOWN_SHA256 = "0999f2f41fda7f97bfaa7589b5ad6504c7ee068795aecd899f98f443e58342bb"
# The revision carried by FocalTech's proprietary Linux driver. It boots the
# sensor too, but configures it for 64x80 instead of 96x96.
KNOWN_SHA256_LINUX = ("907cb638936eb36904eb74e30d4b2b476c25037fdff29666f6bac"
                      "970d326d4b2")

# Every firmware image seen so far opens with an 8051 vector area followed by
# this ramp table. The table is the reliable part: the LJMP targets around it
# differ between images, the table does not.
FW_TABLE_SIG = bytes.fromhex("1e28323c46505a62")
# Offset of that table from the start of the image.
FW_TABLE_OFFSET = 0x11


class CaptureError(Exception):
    """The capture file is not something we can read."""


def _iter_blocks(data: bytes):
    """Yield (block_type, body) for each pcapng block, honouring endianness."""
    if len(data) < 12:
        raise CaptureError("file is too short to be a pcapng capture")

    if struct.unpack_from("<I", data, 0)[0] != BLOCK_SHB:
        raise CaptureError("not a pcapng file (no section header block)")

    magic = struct.unpack_from("<I", data, 8)[0]
    if magic == SHB_BYTE_ORDER_MAGIC:
        end = "<"
    elif struct.unpack_from(">I", data, 8)[0] == SHB_BYTE_ORDER_MAGIC:
        end = ">"
    else:
        raise CaptureError("unrecognised pcapng byte-order magic")

    off = 0
    total = len(data)
    while off + 12 <= total:
        btype, blen = struct.unpack_from(end + "II", data, off)
        # A block is type + length + body + repeated length; 12 bytes minimum,
        # and the length is padded to a multiple of 4.
        if blen < 12 or blen % 4 or off + blen > total:
            raise CaptureError(f"corrupt block of length {blen} at offset {off}")
        yield end, btype, data[off + 8: off + blen - 4]
        off += blen


def _parse_usbpcap(payload: bytes, end: str):
    """Return (device_address, endpoint, is_submit, data) or None."""
    if len(payload) < 2:
        return None
    header_len = struct.unpack_from(end + "H", payload, 0)[0]
    # USBPCAP_BUFFER_PACKET_HEADER is 27 bytes; control transfers append a
    # one-byte stage field.
    if header_len < 27 or header_len > len(payload):
        return None
    # Offsets within USBPCAP_BUFFER_PACKET_HEADER, which is packed and 27
    # bytes long: headerLen(2) irpId(8) status(4) function(2) info(1) bus(2)
    # device(2) endpoint(1) transfer(1) dataLength(4).
    info = payload[16]
    device = struct.unpack_from(end + "H", payload, 19)[0]
    endpoint = payload[21]
    data = payload[header_len:]
    # info bit 0: 0 = FDO -> PDO (host to device, the submit), 1 = completion
    is_submit = not (info & 0x01)
    return device, endpoint, is_submit, data


def _looks_like_image_start(data: bytes, start: int) -> bool:
    """Sanity-check a candidate 8051 image start."""
    if start < 0 or start + FW_TABLE_OFFSET + len(FW_TABLE_SIG) > len(data):
        return False
    # 02 = LJMP at the reset vector, and the same one-byte oddity at offset 6
    # that both known images share.
    return data[start] == 0x02 and data[start + 6] == 0x0B and data[start + 7] == 0x80


def extract_from_binary(data: bytes) -> bytes:
    """Pull the firmware out of the vendor driver binary."""
    starts = []
    off = -1
    while True:
        off = data.find(FW_TABLE_SIG, off + 1)
        if off < 0:
            break
        start = off - FW_TABLE_OFFSET
        if _looks_like_image_start(data, start):
            starts.append(start)

    if not starts:
        raise CaptureError(
            "no 8051 firmware image found in this file. Expected the vendor's "
            "Windows driver (ftUsbWbioDriver.dll) or their Linux libfprint "
            "build (libfprint-2.so.2.0.0). Note that the Windows matching "
            "engine, ftWbioEngineAdapter.dll, contains no firmware."
        )

    print(f"  found {len(starts)} firmware image(s) at "
          + ", ".join(f"0x{s:x}" for s in starts), file=sys.stderr)

    start = starts[0]
    if len(starts) > 1:
        # The vendor driver stores the images back to back, so the next one
        # marks the end of this one.
        length = starts[1] - start
    else:
        # Only one image: take it up to its trailing padding.
        tail = data[start:]
        end = len(tail)
        while end > 0 and tail[end - 1] == 0:
            end -= 1
        length = end
        print("  warning: only one image present, so its length is inferred "
              "from trailing padding", file=sys.stderr)

    # The vendor driver uploads this rounded up to a whole number of 64-byte
    # units, which spills a few bytes of whatever follows. Reproduce that
    # exactly rather than zero-padding, because that is what was verified on
    # hardware.
    padded = -(-length // CHUNK_SIZE) * CHUNK_SIZE
    if start + padded > len(data):
        raise CaptureError(
            f"image at 0x{start:x} claims {length} bytes but the file ends first"
        )
    print(f"  image is {length} bytes, uploaded as {padded}", file=sys.stderr)
    return data[start:start + padded]


def extract(path: Path) -> bytes:
    with path.open("rb") as handle:
        raw = handle.read()

    if len(raw) < 4:
        raise CaptureError("file is too short to be anything useful")

    # Tell the two accepted inputs apart by their magic rather than by file
    # extension, so a renamed file still works.
    if struct.unpack_from("<I", raw, 0)[0] != BLOCK_SHB:
        if raw[:2] == b"MZ":
            print("input looks like a Windows binary", file=sys.stderr)
        else:
            print("input is not a pcapng capture; searching it as a binary",
                  file=sys.stderr)
        return extract_from_binary(raw)

    print("input looks like a pcapng capture", file=sys.stderr)
    linktypes: list[int] = []
    # device address -> list of bulk-OUT payloads, in capture order
    streams: dict[int, list[bytes]] = {}

    for end, btype, body in _iter_blocks(raw):
        if btype == BLOCK_IDB:
            linktypes.append(struct.unpack_from(end + "H", body, 0)[0])
        elif btype == BLOCK_EPB:
            if len(body) < 20:
                continue
            iface, _hi, _lo, cap_len, _orig_len = struct.unpack_from(end + "IIIII", body, 0)
            payload = body[20:20 + cap_len]
            if iface < len(linktypes) and linktypes[iface] != LINKTYPE_USBPCAP:
                continue
            parsed = _parse_usbpcap(payload, end)
            if parsed is None:
                continue
            device, endpoint, is_submit, data = parsed
            if endpoint == FT9201_BULK_OUT_EP and is_submit and data:
                streams.setdefault(device, []).append(data)

    if not linktypes:
        raise CaptureError("capture contains no interface description block")
    if LINKTYPE_USBPCAP not in linktypes:
        raise CaptureError(
            "capture is not USB traffic captured with USBPcap "
            f"(link types present: {sorted(set(linktypes))})"
        )
    if not streams:
        raise CaptureError(
            "no bulk-OUT transfers to endpoint 0x02 found. The capture has to "
            "include the moment the sensor was plugged in, on the root hub it "
            "was plugged into."
        )

    candidates: list[bytes] = []
    for device, payloads in sorted(streams.items()):
        # The download is the long run of equally sized chunks. Everything
        # else on this endpoint (the 4-byte probe word) is shorter.
        run: list[bytes] = []
        best: list[bytes] = []
        for payload in payloads:
            if len(payload) == CHUNK_SIZE:
                run.append(payload)
            else:
                if len(run) > len(best):
                    best = run
                run = []
        if len(run) > len(best):
            best = run
        if best:
            blob = b"".join(best)
            print(f"  device {device}: {len(best)} chunks, {len(blob)} bytes",
                  file=sys.stderr)
            candidates.append(blob)

    if not candidates:
        raise CaptureError(
            f"found bulk-OUT traffic but no run of {CHUNK_SIZE}-byte chunks, "
            "so no firmware download is present in this capture"
        )

    # Every cold plug uploads the same image; if the capture holds several,
    # they must agree, which is a useful integrity check in itself.
    unique = {blob for blob in candidates}
    if len(unique) > 1:
        sizes = sorted({len(b) for b in candidates})
        print(f"warning: {len(unique)} different images were uploaded in this "
              f"capture (sizes {sizes}); using the most common one",
              file=sys.stderr)
        blob = max(unique, key=lambda b: candidates.count(b))
    else:
        blob = candidates[0]

    if not MIN_FW_SIZE <= len(blob) <= MAX_FW_SIZE:
        raise CaptureError(
            f"extracted {len(blob)} bytes, which is outside the plausible "
            f"range {MIN_FW_SIZE}-{MAX_FW_SIZE}"
        )
    if len(blob) % CHUNK_SIZE:
        raise CaptureError(
            f"extracted {len(blob)} bytes, not a multiple of {CHUNK_SIZE}"
        )
    return blob


# ---------------------------------------------------------------------------
# The Microsoft Update Catalog route
# ---------------------------------------------------------------------------
#
# Microsoft publishes the same vendor driver in their Update Catalog. The
# catalog is public: it needs no account, no licence key and no Windows
# machine. Thus a Linux user can get the firmware with one command.
#
# This code does not redistribute the image. It only automates the steps
# that a person does in a browser: a search, a download dialog, and then
# the file itself. Other Linux tools use the same method for other
# devices, for example b43-fwcutter for Broadcom.
#
# The three steps are:
#   1. Search the catalog for the hardware ID of the sensor. Each result
#      row has an update ID of 36 characters.
#   2. Send that update ID to DownloadDialog.aspx. The answer holds the
#      address of a .cab file on the servers of Windows Update.
#   3. Get the .cab and take ftUsbWbioDriver.dll from it.

CATALOG_SEARCH = ("https://www.catalog.update.microsoft.com/Search.aspx"
                  "?q=USB%5CVID_2808%26PID_93A9")
CATALOG_DIALOG = "https://catalog.update.microsoft.com/DownloadDialog.aspx"
CATALOG_UA = "Mozilla/5.0 (X11; Linux x86_64)"
CATALOG_TIMEOUT = 60
DRIVER_MEMBER = "ftUsbWbioDriver.dll"


def _http(url: str, data: bytes | None = None) -> bytes:
    request = urllib.request.Request(url, data=data,
                                     headers={"User-Agent": CATALOG_UA})
    with urllib.request.urlopen(request, timeout=CATALOG_TIMEOUT) as response:
        return response.read()


def _catalog_update_ids(page: str) -> list[str]:
    """Gives the update IDs of the result rows, in their order on the page."""
    ids: list[str] = []
    for match in re.finditer(r'id="([0-9a-f-]{36})_R\d+"', page):
        if match.group(1) not in ids:
            ids.append(match.group(1))
    return ids


def _catalog_cab_url(update_id: str) -> str | None:
    """Gives the address of the .cab file for one update ID."""
    payload = json.dumps([{"size": 0, "languages": "",
                           "uidInfo": update_id, "updateID": update_id}])
    body = urllib.parse.urlencode({"updateIDs": payload}).encode()
    answer = _http(CATALOG_DIALOG, body).decode("utf-8", "replace")
    found = re.search(r"https?://[^'\"]+\.cab", answer)
    return found.group(0) if found else None


def download_driver_cab(destination: Path) -> Path:
    """Takes the vendor driver from the Microsoft Update Catalog.

    Writes ftUsbWbioDriver.dll into the directory `destination` and gives
    its path. Raises CaptureError when a step gives no result, because
    each step depends on a page of Microsoft that can change.
    """
    print("searching the Microsoft Update Catalog for USB\\VID_2808&PID_93A9",
          file=sys.stderr)
    try:
        page = _http(CATALOG_SEARCH).decode("utf-8", "replace")
    except OSError as exc:
        raise CaptureError(f"the catalog search failed: {exc}") from exc

    update_ids = _catalog_update_ids(page)
    if not update_ids:
        raise CaptureError(
            "the catalog gave no result for this hardware ID.\n"
            "       Open " + CATALOG_SEARCH + " in a browser to see the\n"
            "       reason, and use a local driver file instead.")
    print(f"  found {len(update_ids)} catalog entries for this sensor",
          file=sys.stderr)

    cab_url = None
    for update_id in update_ids[:5]:
        try:
            cab_url = _catalog_cab_url(update_id)
        except OSError as exc:
            raise CaptureError(f"the download dialog failed: {exc}") from exc
        if cab_url:
            break
    if not cab_url:
        raise CaptureError("the catalog gave no .cab address for this driver")

    print(f"  downloading {cab_url.rsplit('/', 1)[-1]}", file=sys.stderr)
    try:
        cab_bytes = _http(cab_url)
    except OSError as exc:
        raise CaptureError(f"the download failed: {exc}") from exc
    print(f"  the cabinet has {len(cab_bytes)} bytes", file=sys.stderr)

    cab_path = destination / "ftUsbWbioDriver.cab"
    cab_path.write_bytes(cab_bytes)
    return _extract_cab_member(cab_path, DRIVER_MEMBER, destination)


def _extract_cab_member(cab: Path, member: str, destination: Path) -> Path:
    """Takes one file out of a cabinet, with the tools of the system.

    Fedora and Debian supply cabextract. 7z and bsdtar also read a
    cabinet. This function tries each of them, and gives a clear message
    when the system has none.
    """
    import shutil
    import subprocess

    attempts = [
        ("cabextract", ["cabextract", "-q", "-d", str(destination),
                        "-F", member, str(cab)]),
        ("7z", ["7z", "x", "-y", f"-o{destination}", str(cab), member]),
        ("bsdtar", ["bsdtar", "-x", "-f", str(cab), "-C", str(destination),
                    member]),
    ]
    for tool, command in attempts:
        if not shutil.which(tool):
            continue
        subprocess.run(command, check=False, stdout=subprocess.DEVNULL,
                       stderr=subprocess.DEVNULL)
        result = destination / member
        if result.is_file():
            return result

    raise CaptureError(
        "no tool of this system can open a Microsoft cabinet.\n"
        "       Install one of cabextract, p7zip or bsdtar. On Fedora:\n"
        "         sudo dnf install cabextract\n"
        f"       The cabinet is at {cab}")


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Extract FT9201 MCU firmware from the vendor Windows "
                    "driver (ftUsbWbioDriver.dll) or from a USBPcap capture "
                    "of it running.")
    parser.add_argument("source", type=Path, nargs="?",
                        help="ftUsbWbioDriver.dll, the vendor libfprint-2.so, "
                             "or a .pcapng capture. Omit it with --download.")
    parser.add_argument("-o", "--output", type=Path, default=Path("ft9201.bin"),
                        help="where to write the image (default: ft9201.bin)")
    parser.add_argument("--download", action="store_true",
                        help="take the vendor driver from the Microsoft "
                             "Update Catalog, which is public and needs no "
                             "Windows machine")
    args = parser.parse_args()

    if args.download and args.source is not None:
        print("error: give a source file or --download, and not both",
              file=sys.stderr)
        return 1
    if not args.download and args.source is None:
        parser.print_usage(sys.stderr)
        print("error: give a source file, or use --download", file=sys.stderr)
        return 1

    if not args.download and not args.source.is_file():
        print(f"error: {args.source} is not a file", file=sys.stderr)
        return 1

    try:
        if args.download:
            with tempfile.TemporaryDirectory(prefix="ft9201-") as work:
                driver = download_driver_cab(Path(work))
                blob = extract(driver)
        else:
            blob = extract(args.source)
    except CaptureError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1

    digest = hashlib.sha256(blob).hexdigest()
    with args.output.open("wb") as handle:
        handle.write(blob)

    print(f"wrote {args.output} ({len(blob)} bytes)")
    print(f"sha256 {digest}")
    if digest == KNOWN_SHA256:
        print("this matches the image verified on real hardware by this "
              "project (reports a 96x96 sensor)")
    elif digest == KNOWN_SHA256_LINUX:
        print("this is the revision from FocalTech's Linux driver, also "
              "confirmed to boot\n"
              "the sensor -- but it configures it for 64x80 rather than "
              "96x96. Prefer the\n"
              "Windows driver's image if you can get it.")
    else:
        print("note: this matches neither image this project verified.\n"
              "      That is expected for a different vendor driver revision; "
              "the driver\n"
              "      will tell you if the sensor does not accept it.")
    print(f"\ninstall it with:\n"
          f"  sudo install -D -m 0644 {args.output} "
          f"/lib/firmware/focaltech/ft9201.bin")
    return 0


if __name__ == "__main__":
    sys.exit(main())
