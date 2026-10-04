#!/usr/bin/env bash
# scripts/preflash-check.sh <dev|release>
#
# The pre-flash checks of docs/os/guides/build-flash-provision.md ("Pre-flash checks").
# Exits non-zero, which stops scripts/build.sh, if any check fails:
#
#   1  hook ids found in upstream files equal the table in UPSTREAM-HOOKS.md
#   2  the signing calls appear only in src/identity/, src/hal/se050_apdu.cpp, src/vk/wallet/signer.cpp
#      (Monocypher's own signing functions count as signing calls; the vendored library in
#      src/vk/wallet/vendor/ defines them and is not searched)
#   3  VK_SIGN_DOMAIN( appears only in src/vk/features/*/domain_*.cpp
#   4  nothing under src/native_apps/ or src/vk/features/ includes anything from src/identity/
#   5  test/host/run.sh passes (skipped while that file does not exist)
#   6  release only: src/vk/vk_profile.h defines VK_PROFILE_DEV 0
#
# Checks 2 and 3 ignore text after "//" on a line, so a comment may name these calls.
# Works from any directory. VK_PREFLASH_SKIP_HOST_TESTS=1 skips check 5 (for a quick look at the
# source checks only; scripts/build.sh never sets it).

set -u
set -o pipefail
export LC_ALL=C

FW="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

PROFILE="${1:-}"
case "$PROFILE" in
  dev|release) ;;
  *) echo "usage: scripts/preflash-check.sh <dev|release>" >&2; exit 2 ;;
esac

cd "$FW" || { echo "[preflash] cannot enter $FW" >&2; exit 2; }

TMP="$(mktemp -d "${TMPDIR:-/tmp}/vk-preflash.XXXXXX")" || exit 2
trap 'rm -rf "$TMP"' EXIT

FAILED=0
pass() { echo "[preflash] $1 ok: $2"; }
fail() { echo "[preflash] $1 FAIL: $2" >&2; FAILED=1; }
skip() { echo "[preflash] $1 skipped: $2"; }

# Every firmware source file: the sketch and everything under src/, one path per line.
sources() {
  find os.ino src -type f \( -name '*.ino' -o -name '*.cpp' -o -name '*.c' -o -name '*.h' -o -name '*.hpp' \)
}

# ids in natural order on one line: H1 H2 ... H8a H8b ... H20
one_line() { sort -t H -k 2n "$1" | tr '\n' ' ' | sed 's/ *$//'; }

# --- 1: hook ids ------------------------------------------------------------------------------
# The grep of upstream-hooks.md ("Checking the hooks"), with the exclusion anchored to the file
# path: the H1 line itself contains the text "src/vk/".
check_hooks() {
  if [ ! -f UPSTREAM-HOOKS.md ]; then
    fail 1 "UPSTREAM-HOOKS.md is missing"
    return
  fi

  grep -rn "// VK: H" os.ino src | grep -v '^src/vk/' \
    | sed -E 's/.*VK: (H[0-9]+[a-z]?).*/\1/' | sort -u > "$TMP/found"

  # Table rows are "| H<n> | file | purpose |". A purpose that names a range of sites
  # ("H8a–H8f") stands for those ids; a purpose that starts with "optional:" may be absent.
  awk '
    /^\| *H[0-9]+ *\|/ {
      split($0, col, "|")
      id = col[2]; gsub(/[ \t]/, "", id)
      purpose = col[4]; sub(/^[ \t]+/, "", purpose)
      kind = (purpose ~ /^optional/) ? "optional" : "required"
      if (match(purpose, /H[0-9]+[a-z][^H ]+H[0-9]+[a-z]/)) {
        range = substr(purpose, RSTART, RLENGTH)
        match(range, /^H[0-9]+[a-z]/)
        base = substr(range, 1, RLENGTH - 1)
        from = index(letters, substr(range, RLENGTH, 1))
        to = index(letters, substr(range, length(range), 1))
        for (i = from; i <= to; ++i) print base substr(letters, i, 1), kind
      } else {
        print id, kind
      }
    }
  ' letters=abcdefghijklmnopqrstuvwxyz UPSTREAM-HOOKS.md > "$TMP/table"

  if [ ! -s "$TMP/table" ]; then
    fail 1 "UPSTREAM-HOOKS.md holds no hook table"
    return
  fi

  awk '{ print $1 }' "$TMP/table" | sort -u > "$TMP/allowed"
  awk '$2 == "required" { print $1 }' "$TMP/table" | sort -u > "$TMP/required"
  comm -23 "$TMP/required" "$TMP/found" > "$TMP/missing"
  comm -13 "$TMP/allowed" "$TMP/found" > "$TMP/unknown"

  echo "[preflash] 1 hook ids found: $(one_line "$TMP/found")"
  if [ -s "$TMP/missing" ] || [ -s "$TMP/unknown" ]; then
    [ -s "$TMP/missing" ] && fail 1 "in UPSTREAM-HOOKS.md but not in the source: $(one_line "$TMP/missing")"
    [ -s "$TMP/unknown" ] && fail 1 "in the source but not in UPSTREAM-HOOKS.md: $(one_line "$TMP/unknown")"
    return
  fi
  pass 1 "hook ids equal the table in UPSTREAM-HOOKS.md"
}

# --- 2: one signing path ----------------------------------------------------------------------
check_signing_calls() {
  sources | grep -v -e '^src/identity/' -e '^src/hal/se050_apdu\.cpp$' -e '^src/vk/wallet/signer\.cpp$' \
      -e '^src/vk/wallet/vendor/' \
    | tr '\n' '\0' | xargs -0 awk '
      {
        code = $0; sub(/\/\/.*/, "", code)
        if (code ~ /identity::sign|signBase64|se050_apdu::signEd25519|ed25519::sign|crypto_sign\(|crypto_ed25519_sign\(|crypto_eddsa_sign\(/)
          printf "%s:%d: %s\n", FILENAME, FNR, $0
      }' > "$TMP/signing"
  if [ -s "$TMP/signing" ]; then
    fail 2 "a signing call outside src/identity/, src/hal/se050_apdu.cpp and src/vk/wallet/signer.cpp:"
    sed 's/^/    /' "$TMP/signing" >&2
    return
  fi
  pass 2 "signing calls only in the identity folder, se050_apdu.cpp and wallet/signer.cpp"
}

# --- 3: signing domains only in a feature's domain file ---------------------------------------
check_sign_domains() {
  sources | grep -v -E '^src/vk/features/[^/]+/domain_[^/]*\.cpp$' \
    | tr '\n' '\0' | xargs -0 awk '
      {
        code = $0; sub(/\/\/.*/, "", code)
        if (code ~ /^[ \t]*#[ \t]*define[ \t]+VK_SIGN_DOMAIN\(/) next    # the macro itself (wallet/signer.h)
        if (code ~ /VK_SIGN_DOMAIN\(/) printf "%s:%d: %s\n", FILENAME, FNR, $0
      }' > "$TMP/domains"
  if [ -s "$TMP/domains" ]; then
    fail 3 "VK_SIGN_DOMAIN( outside src/vk/features/*/domain_*.cpp:"
    sed 's/^/    /' "$TMP/domains" >&2
    return
  fi
  pass 3 "VK_SIGN_DOMAIN( only in src/vk/features/*/domain_*.cpp"
}

# --- 4: features and native apps never include the identity folder ----------------------------
check_identity_includes() {
  : > "$TMP/includes"
  for dir in src/native_apps src/vk/features; do
    [ -d "$dir" ] || continue
    grep -rnE '^[[:space:]]*#[[:space:]]*include[[:space:]]*["<][^">]*identity/' "$dir" >> "$TMP/includes"
  done
  if [ -s "$TMP/includes" ]; then
    fail 4 "an include of src/identity/ under src/native_apps/ or src/vk/features/ (use vk::wallet::publicKey()):"
    sed 's/^/    /' "$TMP/includes" >&2
    return
  fi
  pass 4 "no include of src/identity/ under src/native_apps/ or src/vk/features/"
}

# --- 5: host tests ----------------------------------------------------------------------------
check_host_tests() {
  if [ ! -f test/host/run.sh ]; then
    skip 5 "test/host/run.sh does not exist yet"
    return
  fi
  if [ "${VK_PREFLASH_SKIP_HOST_TESTS:-0}" = "1" ]; then
    skip 5 "VK_PREFLASH_SKIP_HOST_TESTS=1"
    return
  fi
  if bash test/host/run.sh > "$TMP/host.log" 2>&1; then
    pass 5 "test/host/run.sh passed ($(grep -c 'tests passed' "$TMP/host.log") suites)"
  else
    fail 5 "test/host/run.sh failed; the end of its output:"
    tail -n 30 "$TMP/host.log" | sed 's/^/    /' >&2
  fi
}

# --- 6: a release build carries the release profile -------------------------------------------
check_release_profile() {
  if [ "$PROFILE" != "release" ]; then
    skip 6 "release only"
    return
  fi
  if grep -Eq '^[[:space:]]*#[[:space:]]*define[[:space:]]+VK_PROFILE_DEV[[:space:]]+0[[:space:]]*$' src/vk/vk_profile.h 2>/dev/null; then
    pass 6 "src/vk/vk_profile.h defines VK_PROFILE_DEV 0"
  else
    fail 6 "src/vk/vk_profile.h does not define VK_PROFILE_DEV 0: a dev build must not go on a judge badge"
  fi
}

check_hooks
check_signing_calls
check_sign_domains
check_identity_includes
check_host_tests
check_release_profile

if [ "$FAILED" -ne 0 ]; then
  echo "[preflash] FAILED ($PROFILE)" >&2
  exit 1
fi
echo "[preflash] all checks passed ($PROFILE)"
