#!/bin/bash
# Takes the matcher of the driver and writes it to an include file.
#
# The offline harness ft9201-matcher.c then tests the code of the driver
# itself. An earlier version of the harness held a copy of the matcher.
# That copy became old, and it measured an algorithm that the driver no
# longer used.
#
# The script reads between two section markers. Thus it stays correct when
# the line numbers change.
set -euo pipefail

here="$(dirname "$0")"
out="${2:-$here/ft9201-matcher-impl.inc}"

# The driver is in the parent directory in the development tree, and in
# driver/ in the published repository.
if [ -n "${1:-}" ]; then
  src="$1"
elif [ -f "$here/../driver/focaltech_ft9201.c" ]; then
  src="$here/../driver/focaltech_ft9201.c"
else
  src="$here/../focaltech_ft9201.c"
fi

if [ ! -f "$src" ]; then
  echo "extract-matcher: no such file: $src" >&2
  exit 1
fi

awk '
  /^\/\*+ MATCHER \*+\/$/ { on = 1 }
  /^\/\*+ TEMPLATE STORAGE \*+\/$/  { on = 0 }
  on { print }
' "$src" > "$out"

lines=$(wc -l < "$out")
if [ "$lines" -lt 100 ]; then
  echo "extract-matcher: found only $lines lines; the markers changed" >&2
  exit 1
fi

echo "extract-matcher: wrote $lines lines to $out"
