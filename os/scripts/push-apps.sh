#!/usr/bin/env bash
# Installs the Lua apps of apps/ on a badge (docs/os/guides/build-flash-provision.md, "Installing apps").
#
#   scripts/push-apps.sh --port /dev/cu.usbserial-10 dev                  every app, over USB serial
#   scripts/push-apps.sh --host 192.168.4.31 --token 123456 release       over Wi-Fi, without the dev-only apps
#
# Every folder under apps/ that holds an app.ini is an app; this script names none of them. What
# it does with each one comes from that app's own app.ini (docs/os/platform/app-host.md, "Manifest"):
#
#   profile=dev      installed by `dev` only; `release` leaves the app out (test fixtures, demos)
#   include=<app>    the files of apps/<app>/ are pushed with this app too, except its config.lua
#                    and app.ini, and except any file this app has itself (the evil game is the
#                    game's code with its own config.lua)
#
# Upstream's push writes only under /apps/<id>/, so the shared library cannot live in one place on
# the badge: each app is pushed from a temporary copy that also holds lib/vk.lua as vk.lua.
#
#   --port <port>    push over USB serial with scripts/push_serial.py, in one session that holds the
#                    port exclusively (see that file for why). A dev build gives it its pairing code;
#                    for a release build add --token <code>. The Python is the repository's venv
#                    (<repo>/.venv/bin/python, which has pyserial), or $VK_PYTHON.
#   --host <ip>      push over Wi-Fi with upstream's tools/badge-push.py; needs --token <code>
#   --token <code>   the six-digit pairing code from Settings -> Push on the badge
#                    (with --host it may also come from the environment variable BADGE_TOKEN)
#   --dry-run        list what would be pushed and push nothing
#   --image <file>   push nothing: write a LittleFS image of the filesystem partition holding every
#                    app under /apps/<id>/, for build.sh to flash with the firmware. Flashing it
#                    replaces the whole filesystem (history, contacts and consents start empty).
#
# Exit status: 0 if every app was pushed, 1 if any push failed, 2 for a usage error.
# Works from any directory; the repository path may contain spaces.
set -u

FW="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
REPO="$(cd "$FW/.." && pwd)"
PY="${VK_PYTHON:-$REPO/.venv/bin/python}"

usage() {
  cat >&2 <<'EOF'
usage: push-apps.sh --port <port> [--token <code>] [--dry-run] dev|release
       push-apps.sh --image <file> dev|release
       push-apps.sh --host <ip> --token <code> [--dry-run] dev|release
EOF
  exit 2
}

port=""
host=""
token="${BADGE_TOKEN:-}"
token_given=0
profile=""
dry_run=0
image=""

while [ "$#" -gt 0 ]; do
  case "$1" in
    --port)    [ "$#" -ge 2 ] || usage; port="$2"; shift 2 ;;
    --host)    [ "$#" -ge 2 ] || usage; host="$2"; shift 2 ;;
    --token)   [ "$#" -ge 2 ] || usage; token="$2"; token_given=1; shift 2 ;;
    --dry-run) dry_run=1; shift ;;
    --image)   [ "$#" -ge 2 ] || usage; image="$2"; shift 2 ;;
    dev|release)
      [ -z "$profile" ] || usage
      profile="$1"; shift ;;
    -h|--help) usage ;;
    *) echo "push-apps.sh: unknown argument: $1" >&2; usage ;;
  esac
done

[ -n "$profile" ] || usage
if [ -n "$port" ] && [ -n "$host" ]; then
  echo "push-apps.sh: give --port or --host, not both" >&2
  usage
fi
if [ -z "$port" ] && [ -z "$host" ] && [ -z "$image" ]; then
  echo "push-apps.sh: give --port <port> or --host <ip>" >&2
  usage
fi
if [ -n "$host" ] && [ -z "$token" ]; then
  echo "push-apps.sh: --host needs --token <code> (Settings -> Push on the badge)" >&2
  usage
fi

if [ ! -f "$FW/lib/vk.lua" ]; then
  echo "push-apps.sh: $FW/lib/vk.lua is missing" >&2
  exit 1
fi

if [ "$dry_run" -eq 0 ] && [ -z "$image" ]; then
  if [ -n "$port" ]; then
    if [ ! -x "$PY" ]; then
      echo "push-apps.sh: $PY not found; from the repository root run:" >&2
      echo "  python3 -m venv .venv && .venv/bin/pip install pyserial esptool" >&2
      echo "or point VK_PYTHON at a Python that has pyserial" >&2
      exit 1
    fi
  else
    # badge-push.py needs only the standard library over Wi-Fi.
    [ -x "$PY" ] || PY="$(command -v python3 || true)"
    if [ -z "$PY" ]; then
      echo "push-apps.sh: no python3 found" >&2
      exit 1
    fi
  fi
fi

TMP="$(mktemp -d "${TMPDIR:-/tmp}/push-apps.XXXXXX")" || exit 1
trap 'rm -rf "$TMP"' EXIT

# manifest_key <id> <key>: the value of `key` in apps/<id>/app.ini (the last line wins, as on the
# badge), spaces around it dropped; empty when the key is absent.
manifest_key() {
  sed -n -E "s/^[[:space:]]*$2[[:space:]]*=[[:space:]]*(.*[^[:space:]])?[[:space:]]*\$/\\1/p" \
    "$FW/apps/$1/app.ini" | tr -d '\r' | tail -n 1
}

# prepare <id>: fills $TMP/<id> with what is pushed for that app. Returns 1 if it cannot.
prepare() {
  local id="$1" work="$TMP/$1" include f name
  mkdir -p "$work" || return 1
  cp -R "$FW/apps/$id/." "$work/" || return 1
  include="$(manifest_key "$id" include)"
  if [ -n "$include" ]; then
    if [ ! -f "$FW/apps/$include/app.ini" ]; then
      echo "push-apps.sh: $id includes apps/$include, which is not an app" >&2
      return 1
    fi
    for f in "$FW/apps/$include"/*; do
      [ -f "$f" ] || continue
      name="$(basename "$f")"
      case "$name" in config.lua|app.ini) continue ;; esac
      [ -e "$work/$name" ] && continue          # the app's own file wins
      cp "$f" "$work/" || return 1
    done
  fi
  cp "$FW/lib/vk.lua" "$work/vk.lua" || return 1
}

pushed=()
skipped=()
failed=()
serial_args=()

for dir in "$FW"/apps/*/; do
  [ -f "$dir/app.ini" ] || continue
  id="$(basename "$dir")"
  if [ "$profile" = "release" ] && [ "$(manifest_key "$id" profile)" = "dev" ]; then
    skipped+=("$id")
    continue
  fi
  if ! prepare "$id"; then
    echo "push-apps.sh: $id could not be prepared" >&2
    failed+=("$id")
    continue
  fi
  if [ -n "$image" ]; then
    mkdir -p "$TMP/.image/apps" && cp -R "$TMP/$id" "$TMP/.image/apps/$id" && pushed+=("$id") || failed+=("$id")
  elif [ "$dry_run" -eq 1 ]; then
    echo "== $id"
    (cd "$TMP/$id" && find . -type f ! -name '.*' | sed 's|^\./|    |' | sort)
    pushed+=("$id")
  elif [ -n "$port" ]; then
    serial_args+=("$id=$TMP/$id")                # all of them in one session, below
  else
    echo "== $id"
    # --id: the tool would otherwise take the id from the folder it is given.
    if "$PY" "$FW/tools/badge-push.py" --host "$host" --token "$token" push "$TMP/$id" --id "$id"; then
      pushed+=("$id")
    else
      echo "push-apps.sh: $id failed" >&2
      failed+=("$id")
    fi
  fi
done

if [ "${#serial_args[@]}" -gt 0 ]; then
  code_args=()
  [ "$token_given" -eq 1 ] && code_args=(--code "$token")
  log="$TMP/push.log"
  "$PY" "$FW/scripts/push_serial.py" --port "$port" ${code_args[@]+"${code_args[@]}"} "${serial_args[@]}" 2>&1 | tee "$log"
  # An app counts as pushed only when push_serial.py said so; a crash leaves the rest failed.
  for arg in "${serial_args[@]}"; do
    id="${arg%%=*}"
    if grep -q -x -F "push: $id ok" "$log"; then
      pushed+=("$id")
    else
      failed+=("$id")
    fi
  done
fi

if [ -n "$image" ]; then
  # The filesystem partition of partitions.csv (`spiffs`, mounted as LittleFS), with the Arduino
  # core's LittleFS geometry: 4096-byte blocks, 256-byte pages.
  size="$(awk -F, '$1 ~ /^spiffs/ {gsub(/ /, "", $5); print $5}' "$FW/partitions.csv")"
  tool="${MKLITTLEFS:-$(ls -d "$HOME"/Library/Arduino15/packages/esp32/tools/mklittlefs/*/mklittlefs "$HOME"/.arduino15/packages/esp32/tools/mklittlefs/*/mklittlefs 2>/dev/null | tail -n 1)}"
  if [ -z "$tool" ] || [ ! -x "$tool" ] || [ -z "$size" ]; then
    echo "push-apps.sh: mklittlefs or the spiffs partition size not found (set MKLITTLEFS)" >&2
    exit 1
  fi
  mkdir -p "$TMP/.image/apps"
  "$tool" -c "$TMP/.image" -b 4096 -p 256 -s "$((size))" "$image" > /dev/null || exit 1
  verb="put in $image"
fi
verb="${verb:-pushed}"
[ "$dry_run" -eq 1 ] && verb="would push"
echo "push-apps.sh: $profile: $verb ${#pushed[@]} app(s): ${pushed[*]-}"
if [ "${#skipped[@]}" -gt 0 ]; then
  echo "push-apps.sh: left out (profile=dev): ${skipped[*]}"
fi
if [ "${#failed[@]}" -gt 0 ]; then
  echo "push-apps.sh: FAILED: ${failed[*]}" >&2
  exit 1
fi
exit 0
