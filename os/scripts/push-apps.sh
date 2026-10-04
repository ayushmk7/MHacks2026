#!/usr/bin/env bash
# Installs the Lua apps of apps/ on a badge (docs/os/guides/build-flash-provision.md, "Installing apps").
#
#   scripts/push-apps.sh --port /dev/cu.usbserial-10 dev                  every app, over USB serial
#   scripts/push-apps.sh --host 192.168.4.31 --token 123456 release       over Wi-Fi, without the dev-only test apps
#
# Upstream's push writes only under /apps/<id>/, so the shared library cannot live in one place on
# the badge. For each folder under apps/ this script makes a temporary copy, adds lib/vk.lua to it
# as vk.lua, and pushes the copy under the folder's name. The copy of `evilgame` also gets
# apps/game/*.lua except config.lua: the evil game is the game with another config.
#
#   dev       every app
#   release   every app except the dev-only test apps: signtest checktest vktest reqtest
#             (evilgame is included: the demo needs it)
#
#   --port <port>    push over USB serial with scripts/vkdev.py, run by the repository's venv
#                    (<repo>/.venv/bin/python, which has pyserial). A dev build gives vkdev.py its
#                    pairing code itself; for a release build add --token <code>.
#   --host <ip>      push over Wi-Fi with upstream's tools/badge-push.py; needs --token <code>
#   --token <code>   the six-digit pairing code from Settings -> Push on the badge
#                    (with --host it may also come from the environment variable BADGE_TOKEN)
#   --dry-run        list what would be pushed and push nothing
#
# Exit status: 0 if every app was pushed, 1 if any push failed, 2 for a usage error.
# Works from any directory; the repository path may contain spaces.
set -u

FW="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
REPO="$(cd "$FW/.." && pwd)"
PY="$REPO/.venv/bin/python"
DEV_ONLY="signtest checktest vktest reqtest"

usage() {
  cat >&2 <<'EOF'
usage: push-apps.sh --port <port> [--token <code>] [--dry-run] dev|release
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

while [ "$#" -gt 0 ]; do
  case "$1" in
    --port)    [ "$#" -ge 2 ] || usage; port="$2"; shift 2 ;;
    --host)    [ "$#" -ge 2 ] || usage; host="$2"; shift 2 ;;
    --token)   [ "$#" -ge 2 ] || usage; token="$2"; token_given=1; shift 2 ;;
    --dry-run) dry_run=1; shift ;;
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
if [ -z "$port" ] && [ -z "$host" ]; then
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

if [ "$dry_run" -eq 0 ]; then
  if [ -n "$port" ]; then
    if [ ! -x "$PY" ]; then
      echo "push-apps.sh: $PY not found; from the repository root run:" >&2
      echo "  python3 -m venv .venv && .venv/bin/pip install pyserial esptool" >&2
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

is_dev_only() {
  case " $DEV_ONLY " in
    *" $1 "*) return 0 ;;
  esac
  return 1
}

# prepare <id>: fills $TMP/<id> with what is pushed for that app. Returns 1 if it cannot.
prepare() {
  local id="$1" work="$TMP/$1" f
  mkdir -p "$work" || return 1
  cp -R "$FW/apps/$id/." "$work/" || return 1
  if [ "$id" = "evilgame" ]; then
    if [ ! -d "$FW/apps/game" ]; then
      echo "push-apps.sh: evilgame needs apps/game, which is missing" >&2
      return 1
    fi
    for f in "$FW"/apps/game/*.lua; do
      [ -f "$f" ] || continue
      [ "$(basename "$f")" = "config.lua" ] && continue
      cp "$f" "$work/" || return 1
    done
  fi
  cp "$FW/lib/vk.lua" "$work/vk.lua" || return 1
}

# push <id>: sends $TMP/<id> to the badge as app <id>.
push() {
  local id="$1" work="$TMP/$1"
  if [ "$dry_run" -eq 1 ]; then
    (cd "$work" && find . -type f ! -name '.*' | sed 's|^\./|    |' | sort)
    return 0
  fi
  if [ -n "$port" ]; then
    if [ "$token_given" -eq 1 ]; then
      "$PY" "$FW/scripts/vkdev.py" --port "$port" --code "$token" push "$work" --id "$id"
    else
      "$PY" "$FW/scripts/vkdev.py" --port "$port" push "$work" --id "$id"
    fi
  else
    # --id: the tool would otherwise take the id from the folder it is given.
    "$PY" "$FW/tools/badge-push.py" --host "$host" --token "$token" push "$work" --id "$id"
  fi
}

pushed=()
skipped=()
failed=()

for dir in "$FW"/apps/*/; do
  [ -d "$dir" ] || continue
  id="$(basename "$dir")"
  if [ "$profile" = "release" ] && is_dev_only "$id"; then
    skipped+=("$id")
    continue
  fi
  echo "== $id"
  if prepare "$id" && push "$id"; then
    pushed+=("$id")
  else
    echo "push-apps.sh: $id failed" >&2
    failed+=("$id")
  fi
  rm -rf "${TMP:?}/$id"
done

verb="pushed"
[ "$dry_run" -eq 1 ] && verb="would push"
echo "push-apps.sh: $profile: $verb ${#pushed[@]} app(s): ${pushed[*]-}"
if [ "${#skipped[@]}" -gt 0 ]; then
  echo "push-apps.sh: left out (dev only): ${skipped[*]}"
fi
if [ "${#failed[@]}" -gt 0 ]; then
  echo "push-apps.sh: FAILED: ${failed[*]}" >&2
  exit 1
fi
exit 0
