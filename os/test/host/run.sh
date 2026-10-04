#!/usr/bin/env bash
# Host tests for Badge OS: plain C99 / C++17 on the laptop's compiler, no Arduino, no badge.
#
#   test/host/run.sh               every test/host/test_*.c, test_*.cpp and (if `lua` exists) test_*.lua
#   test/host/run.sh sol record    only those suites ("sol", "test_sol" and "test_sol.c" all work)
#
# A suite named test_x must print "all x tests passed" as its last line and exit 0.
# Build output goes to test/host/build/<suite>/.
#
# Every C and C++ suite is linked with all of src/vk/wallet/pure/*.c, test/host/host_ed25519.c and
# src/identity/tweetnacl.c; C++ suites also with test/host/shim/*.cpp. A suite names any further
# firmware sources in a comment within its first five lines (paths relative to the firmware folder,
# space-separated):
#   // LINK: src/vk/core/config.cpp src/vk/wallet/approval.cpp
#
# Flags:
#   C    cc  -std=c99   -Wall -Wextra -Wpedantic -O2 -DSOL_HOST_SHA256 -DVK_HOST_TEST
#   C++  c++ -std=c++17 -Wall -Wextra -O2 -DSOL_HOST_SHA256 -DVK_HOST_TEST -Itest/host/shim -Itest/host
# Lua suites run as `lua test/host/test_x.lua` from the firmware folder.
#
# Exit status: 0 if every suite passed, 1 otherwise. Works from any directory.
set -u

FW="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$FW" || exit 1

CC="${CC:-cc}"
CXX="${CXX:-c++}"
CFLAGS=(-std=c99 -Wall -Wextra -Wpedantic -O2 -DSOL_HOST_SHA256 -DVK_HOST_TEST)
CXXFLAGS=(-std=c++17 -Wall -Wextra -O2 -DSOL_HOST_SHA256 -DVK_HOST_TEST -Itest/host/shim -Itest/host)
HOST=test/host
BUILD="$HOST/build"

have_lua=0
command -v lua >/dev/null 2>&1 && have_lua=1

# ---- which suites ---------------------------------------------------------------------------
suites=()                      # entries are source paths, e.g. test/host/test_sol.c
if [ "$#" -eq 0 ]; then
  for f in "$HOST"/test_*.c "$HOST"/test_*.cpp; do
    [ -f "$f" ] && suites+=("$f")
  done
  if [ "$have_lua" -eq 1 ]; then
    for f in "$HOST"/test_*.lua; do
      [ -f "$f" ] && suites+=("$f")
    done
  fi
else
  for arg in "$@"; do
    name="$(basename "$arg")"
    name="${name%.c}"; name="${name%.cpp}"; name="${name%.lua}"
    case "$name" in test_*) ;; *) name="test_$name" ;; esac
    found=""
    for ext in c cpp lua; do
      if [ -f "$HOST/$name.$ext" ]; then found="$HOST/$name.$ext"; break; fi
    done
    if [ -z "$found" ]; then
      echo "run.sh: no suite $name (looked for $HOST/$name.c, .cpp, .lua)" >&2
      exit 1
    fi
    suites+=("$found")
  done
fi

if [ "${#suites[@]}" -eq 0 ]; then
  echo "run.sh: no suites found in $HOST" >&2
  exit 1
fi

# ---- helpers --------------------------------------------------------------------------------
# Prints the paths on the suite's "LINK:" line (first five lines), one per line.
link_sources() {
  head -n 5 "$1" | sed -n 's/^.*LINK:[[:space:]]*//p' | head -n 1 | sed 's/\*\/.*$//' | tr -s ' \t\r' '\n\n\n' | sed '/^$/d'
}

# compile_objects <outdir> <src>...   Compiles each .c / .cpp to <outdir>/<n>_<name>.o; fills OBJS.
OBJS=()
compile_objects() {
  local out="$1"; shift
  local n=0 src obj
  OBJS=()
  for src in "$@"; do
    n=$((n + 1))
    obj="$out/${n}_$(basename "${src%.*}").o"
    case "$src" in
      # Upstream's vendored TweetNaCl is compiled as it is; its sign-compare warnings are not ours to fix.
      src/identity/tweetnacl.c) "$CC" "${CFLAGS[@]}" -w -c "$src" -o "$obj" || return 1 ;;
      *.c)   "$CC"  "${CFLAGS[@]}"   -c "$src" -o "$obj" || return 1 ;;
      *.cpp) "$CXX" "${CXXFLAGS[@]}" -c "$src" -o "$obj" || return 1 ;;
      *)     echo "run.sh: cannot compile $src (not .c or .cpp)" >&2; return 1 ;;
    esac
    OBJS+=("$obj")
  done
}

# run_suite <source path>   Returns 0 if the suite built, ran, exited 0 and printed its pass line.
run_suite() {
  local src="$1"
  local base suite short out bin log expect status last
  base="$(basename "$src")"
  suite="${base%.*}"
  short="${suite#test_}"
  out="$BUILD/$suite"
  bin="$out/$suite"
  log="$out/output.txt"
  expect="all $short tests passed"
  rm -rf "$out"
  mkdir -p "$out" || return 1

  case "$src" in
    *.lua)
      if [ "$have_lua" -ne 1 ]; then
        echo "run.sh: $suite needs a lua binary" >&2
        return 1
      fi
      lua "$src" >"$log" 2>&1
      status=$?
      ;;
    *)
      local common=() extra=() line
      for f in src/vk/wallet/pure/*.c; do
        [ -f "$f" ] && common+=("$f")
      done
      common+=("$HOST/host_ed25519.c" src/identity/tweetnacl.c)
      while IFS= read -r line; do
        if [ ! -f "$line" ]; then
          echo "run.sh: $suite: LINK source $line does not exist" >&2
          return 1
        fi
        extra+=("$line")
      done < <(link_sources "$src")
      case "$src" in
        *.c)
          compile_objects "$out" "$src" "${common[@]}" ${extra[@]+"${extra[@]}"} || return 1
          # A C suite that links a .cpp needs the C++ runtime; otherwise link as C.
          local linker="$CC" e
          for e in ${extra[@]+"${extra[@]}"}; do
            case "$e" in *.cpp) linker="$CXX" ;; esac
          done
          "$linker" "${OBJS[@]}" -o "$bin" || return 1
          ;;
        *.cpp)
          local shim=()
          for f in "$HOST"/shim/*.cpp; do
            [ -f "$f" ] && shim+=("$f")
          done
          compile_objects "$out" "$src" "${common[@]}" ${shim[@]+"${shim[@]}"} ${extra[@]+"${extra[@]}"} || return 1
          "$CXX" "${OBJS[@]}" -o "$bin" || return 1
          ;;
      esac
      "$bin" >"$log" 2>&1
      status=$?
      ;;
  esac

  cat "$log"
  if [ "$status" -ne 0 ]; then
    echo "run.sh: $suite exited with status $status" >&2
    return 1
  fi
  last="$(sed -e 's/\r$//' -e '/^$/d' "$log" | tail -n 1)"
  if [ "$last" != "$expect" ]; then
    echo "run.sh: $suite did not end with \"$expect\"" >&2
    return 1
  fi
  return 0
}

# ---- run ------------------------------------------------------------------------------------
failed=()
for src in "${suites[@]}"; do
  if ! run_suite "$src"; then
    name="$(basename "$src")"
    failed+=("${name%.*}")
  fi
done

if [ "${#failed[@]}" -ne 0 ]; then
  echo "run.sh: FAILED: ${failed[*]}" >&2
  exit 1
fi
exit 0
