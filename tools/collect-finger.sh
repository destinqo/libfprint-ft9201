#!/bin/bash
# Collects frames of one finger into its own directory.
#
# Each directory holds one finger. The tool ft9201-matcher groups the frames
# by the name of the parent directory, thus the name must be different for
# each finger.
#
# The script gives a sound for each frame that the sensor accepts. A person
# who collects frames does not watch the terminal, and a silent tool gives
# no answer to "did that press count?".
#
# Put the finger down in the same way each time, as in normal use. Do not
# change the position on purpose. The sensor is 3 x 4 mm, thus a changed
# position records a different area of the same finger, and two different
# areas do not correlate.
#
# Usage:  collect-finger.sh <name> [count]
set -uo pipefail

T="$(cd "$(dirname "$0")" && pwd)"
root="$T/../samples/dataset-2026-09-09"
name="${1:?usage: collect-finger.sh <name> [count]}"
count="${2:-10}"
dir="$root/$name"

mkdir -p "$dir"
have=$(find "$dir" -name '*.pgm' | wc -l)
if [ "$have" -ge "$count" ]; then
  echo "collect-finger: $dir already holds $have frames" >&2
  exit 1
fi
# The directory can hold frames of an earlier run that stopped. The script
# then collects only the frames that are missing, and it keeps the others.
count=$((count - have))

# fprintd claims the interface again after each stop, because a D-Bus call
# starts it. A mask keeps it stopped for the time of the collection.
restore_fprintd () {
  sudo systemctl unmask fprintd >/dev/null 2>&1
}
trap restore_fprintd EXIT
sudo systemctl stop fprintd >/dev/null 2>&1
sudo systemctl mask fprintd >/dev/null 2>&1

# The sensor keeps no firmware. After a re-connection its MCU is empty.
# The loader sends the image and needs no finger.
if ! sudo /tmp/ft9201-fwload2 /lib/firmware/focaltech/ft9201.bin 2>&1 | grep -q SUCCESS; then
  echo "collect-finger: the firmware did not start; check the cable" >&2
  rmdir "$dir" 2>/dev/null
  exit 1
fi

echo "== $name: $count presses =="
echo "   Three bells: start. One bell: the press counted, press again."
"$T/beep" >/dev/null 2>&1 &

got=0
( cd "$dir" && sudo stdbuf -oL /tmp/ft9201-capture "$count" 2>&1 &&
  i=0; for f in linux-frame*.pgm; do
    [ -e "$f" ] || continue
    while :; do i=$((i+1)); [ -e "$(printf 'frame%02d.pgm' $i)" ] || break; done
    mv "$f" "$(printf 'frame%02d.pgm' $i)"
  done ) |
while IFS= read -r line; do
  case "$line" in
    *wrote\ *)
      got=$((got + 1))
      echo "   frame $got of $count"
      "$T/beep1" >/dev/null 2>&1 &
      ;;
  esac
done

n=$(find "$dir" -name '*.pgm' | wc -l)
echo "   -> $n frames in $dir"
"$T/beep" >/dev/null 2>&1 &
