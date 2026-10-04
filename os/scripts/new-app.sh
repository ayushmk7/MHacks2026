#!/usr/bin/env bash
# scripts/new-app.sh <id> "<Name>" [--native] [--category <name>]
#
# Makes a new app that runs as it is, from os/templates/ (docs/os/guides/extending.md, "Add an app"):
#
#   Lua (default)  apps/<id>/{app.ini,config.lua,main.lua} from templates/lua_app/.
#                  Install it with scripts/push-apps.sh --port <port> dev: nothing else to edit.
#   --native       src/native_apps/<id>/<id>.cpp from templates/native_app/app.cpp.
#                  It self-registers: scripts/build.sh dev --upload <port> and it is on the launcher.
#   --category     the launcher folder to list it in ([a-z0-9_-], at most 16 characters).
#
# To remove an app, delete the folder this made (and `DEL <id>` on a badge that has a Lua app).
#
# <id>: [a-z0-9._-], at most 32 characters, not starting with '.' (what the badge accepts); for a
# native app [a-z0-9_] only, because it also names the C++ class. Refuses an id that is already an
# app, Lua or native. Exit status: 0 made, 1 refused, 2 usage.
set -euo pipefail

FW="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

usage() {
  echo 'usage: scripts/new-app.sh <id> "<Name>" [--native] [--category <name>]' >&2
  exit 2
}

[ "$#" -ge 2 ] || usage
id="$1"
name="$2"
shift 2
native=0
category=""
while [ "$#" -gt 0 ]; do
  case "$1" in
    --native) native=1; shift ;;
    --category) [ "$#" -ge 2 ] || usage; category="$2"; shift 2 ;;
    *) usage ;;
  esac
done

fail() { echo "new-app.sh: $*" >&2; exit 1; }

# The id, as app_store::isValidId() checks it on the badge.
[[ "$id" =~ ^[a-z0-9_-][a-z0-9._-]{0,31}$ ]] || fail "bad id '$id': use [a-z0-9._-], at most 32 characters, not starting with '.'"
if [ "$native" -eq 1 ]; then
  [[ "$id" =~ ^[a-z][a-z0-9_]*$ ]] || fail "a native id names a C++ class too: use [a-z0-9_], starting with a letter"
fi
[ -n "$name" ] || fail "the name is empty"
case "$name" in *[\"\\\&\|]*) fail "the name may not contain \" \\ & or |" ;; esac
if [ -n "$category" ]; then
  [[ "$category" =~ ^[a-z0-9_-]{1,16}$ ]] || fail "bad category '$category': use [a-z0-9_-], at most 16 characters"
fi

# An id is taken if a Lua app or a native app already uses it.
[ ! -e "$FW/apps/$id" ] || fail "apps/$id already exists"
[ ! -e "$FW/src/native_apps/$id" ] || fail "src/native_apps/$id already exists"
if grep -rqs --include='*.cpp' -E "BADGE_APP\([^,]+, *\"$id\"" "$FW/src/native_apps"; then
  fail "a native app already registers the id '$id'"
fi

# The class name: the id in CamelCase ("my_app" -> "MyApp"), plus "App" when that is a clash.
class_name() {
  local out="" part
  IFS='_' read -r -a parts <<< "$1"
  for part in "${parts[@]}"; do
    [ -n "$part" ] && out="$out$(printf '%s' "${part:0:1}" | tr '[:lower:]' '[:upper:]')${part:1}"
  done
  printf '%sApp' "$out"
}

title="$(printf '%s' "$name" | tr '[:lower:]' '[:upper:]')"

if [ "$native" -eq 1 ]; then
  dir="$FW/src/native_apps/$id"
  launcher=""
  [ -n "$category" ] && launcher="category=$category"
  mkdir -p "$dir"
  sed -e "s|__CLASS__|$(class_name "$id")|g" -e "s|__ID__|$id|g" -e "s|__NAME__|$name|g" \
      -e "s|__TITLE__|$title|g" -e "s|__LAUNCHER__|$launcher|g" \
      "$FW/templates/native_app/app.cpp" > "$dir/$id.cpp"
  echo "made src/native_apps/$id/$id.cpp"
  echo "next: scripts/build.sh dev --upload <port>"
else
  dir="$FW/apps/$id"
  mkdir -p "$dir"
  for f in app.ini config.lua main.lua; do
    sed -e "s|__ID__|$id|g" -e "s|__NAME__|$name|g" "$FW/templates/lua_app/$f" > "$dir/$f"
  done
  [ -n "$category" ] && printf 'category=%s\n' "$category" >> "$dir/app.ini"
  echo "made apps/$id/ (app.ini, config.lua, main.lua)"
  echo "next: scripts/push-apps.sh --port <port> dev"
fi
echo "remove it: delete that folder$([ "$native" -eq 0 ] && echo " (and DEL $id on a badge that has it)")"
