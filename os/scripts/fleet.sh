#!/usr/bin/env bash
# Flashes several badges at once with the same firmware and the same apps, then checks that every
# badge shows the same launcher.
#
#   scripts/fleet.sh <dev|release> <port> [<port> ...]
#   scripts/fleet.sh release /dev/cu.usbserial-10 /dev/cu.usbserial-210 /dev/cu.usbserial-310 /dev/cu.usbserial-410
#
# 1. builds the profile once (scripts/build.sh, with its pre-flash checks) and the apps image once
#    (scripts/push-apps.sh --image);
# 2. writes the firmware and the apps image to every port in parallel. The apps image replaces the
#    filesystem, so each badge has exactly the apps of os/apps/ for the profile and nothing else
#    (history, contacts and consents start empty; the key and the settings are kept);
# 3. reads each badge's launcher (VKSTATE menu; dev builds only, a release build has no VKSTATE)
#    and fails if any two differ.
#
# After it: power-cycle every badge once (USB out, switch off, switch on, USB in), or the buttons
# may stay dead (docs/os/guides/flash-another-badge.md).
set -u

FW="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PY="${VK_PYTHON:-$FW/../.venv/bin/python}"

[ "$#" -ge 2 ] || { echo "usage: scripts/fleet.sh <dev|release> <port> [<port> ...]" >&2; exit 2; }
PROFILE="$1"; shift
case "$PROFILE" in dev|release) ;; *) echo "fleet.sh: profile is dev or release" >&2; exit 2 ;; esac
BUILD="$FW/build/$PROFILE"

"$FW/scripts/build.sh" "$PROFILE" || exit 1
"$FW/scripts/push-apps.sh" --image "$BUILD/apps.bin" "$PROFILE" || exit 1
offset="$(awk -F, '$1 ~ /^spiffs/ {gsub(/ /, "", $4); print $4}' "$FW/partitions.csv")"

LOGS="$(mktemp -d "${TMPDIR:-/tmp}/fleet.XXXXXX")"
pids=()
for port in "$@"; do
  (
    cd "$BUILD" &&
    "$PY" -m esptool --chip esp32s3 --port "$port" --baud 460800 write_flash @flash_args "$offset" apps.bin
  ) > "$LOGS/$(basename "$port").log" 2>&1 &
  pids+=("$!")
done
failed=0
i=0
for port in "$@"; do
  if wait "${pids[$i]}"; then
    echo "[fleet] $port flashed"
  else
    echo "[fleet] $port FAILED, see $LOGS/$(basename "$port").log" >&2
    failed=1
  fi
  i=$((i + 1))
done
[ "$failed" -eq 0 ] || exit 1

[ "$PROFILE" = "dev" ] || { echo "[fleet] release: launchers not compared (no VKSTATE); every badge has the same image"; exit 0; }
sleep 8
first=""
for port in "$@"; do
  menu="$("$PY" "$FW/scripts/vkdev.py" --port "$port" menu 2>&1 | tail -n 1)"
  echo "[fleet] $port: $menu"
  if [ -z "$first" ]; then first="$menu"; elif [ "$menu" != "$first" ]; then failed=1; fi
done
if [ "$failed" -ne 0 ]; then echo "[fleet] the launchers differ" >&2; exit 1; fi
echo "[fleet] all $# badges show the same launcher. Now power-cycle each one."
