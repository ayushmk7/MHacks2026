# Host tests

How to build and run the wallet core's pure-C modules on a development computer, with no badge: what the tests cover, where the test vectors come from, and how the tests are laid out in the firmware tree.

- Audience: firmware engineers.
- Status: design, not yet built on hardware.
- Base: Solana OS, directory `firmware/solana-os/` at commit `812b8c7` of the [upstream repository](https://github.com/spacemandev-git/solana-defcon-badge-26/tree/812b8c7aca5c366d18c0b040fafd2999f7204d84/firmware/solana-os).

Tags: **[UPSTREAM]** exists in Solana OS at that commit. **[OURS]** is our design decision. **[UNVERIFIED]** must be confirmed on a badge; a fallback is given.

"Host-tested" in these documents means exactly what this page describes: codec round trips, address derivation, the transaction decoder and builder, and the attestation account parser, run on a laptop against vectors generated with `@solana/kit`, `sas-lib` and `@solana-program/token`. It says nothing about behaviour on the ESP32-S3. Nothing here has run on a badge.

All three suites were last run on 2026-10-03 (macOS, Apple clang 17, with the commands below) and passed.

## What exists today

The reference code is in [`docs/os/reference/code/`](../reference/code/). It is pure C99 with no heap and no Arduino dependency.

| File | Contents | Destination in the firmware tree |
|---|---|---|
| [`sol.h`](../reference/code/sol.h) | API for the `sol_*` modules | `src/wallet/` |
| [`sol_b58.c`](../reference/code/sol_b58.c) | base58 encode and decode | `src/wallet/` |
| [`sol_curve.c`](../reference/code/sol_curve.c) | `sol_is_on_curve()` | `src/wallet/` |
| [`sol_pda.c`](../reference/code/sol_pda.c) | `sol_find_pda`, `sol_ata`, `sol_sas_attestation_pda`, program ids | `src/wallet/` |
| [`sol_tx.c`](../reference/code/sol_tx.c) | strict `TransferChecked` decoder, message builder, amount format and parse | `src/wallet/` |
| [`sol_sha256.c`](../reference/code/sol_sha256.c) | mbedTLS wrapper on the badge; portable SHA-256 when `SOL_HOST_SHA256` is defined | `src/wallet/` |
| [`pay_proto.h`](../reference/code/pay_proto.h), [`pay_proto.c`](../reference/code/pay_proto.c) | codec for the badge-to-badge payment messages | `src/wallet/` |
| [`attest_parse.c`](../reference/code/attest_parse.c) | `attest_parse()`: field checks for one attestation account; declared in `sdk-headers/wallet/attest.h` | `src/wallet/` |
| [`test_sol.c`](../reference/code/test_sol.c) | tests for the `sol_*` modules | `test/host/` |
| [`test_pay.c`](../reference/code/test_pay.c) | tests for the codec, with real Ed25519 signatures | `test/host/` |
| [`test_attest.c`](../reference/code/test_attest.c) | tests for `attest_parse()` | `test/host/` |
| [`vectors.mjs`](../reference/code/vectors.mjs) | vector generator (Node); prints `vectors.json` | `test/host/` |
| [`vectors-to-h.mjs`](../reference/code/vectors-to-h.mjs) | converts `vectors.json` to `vectors.h` | `test/host/` |
| [`vectors.json`](../reference/code/vectors.json), [`vectors.h`](../reference/code/vectors.h) | generated vectors; the header is what the tests include | `test/host/` |
| [`sdk-headers/`](../reference/code/sdk-headers/) | header and example listings, syntax-checked only: `app_host/badge_api.h`; `wallet/` with `wallet.h`, `pay_session.h`, `attest.h`, `wallet_crypto.h`, `wallet_internal.h`, `audit.h`, `history.h` and byte-identical copies of `sol.h` and `pay_proto.h`; `identity/identity_private.h`; `sdk/badge_sdk.hpp`; `native_apps/registry.cpp` and `native_apps/tipjar/tipjar.cpp` | `src/app_host/`, `src/wallet/`, `src/identity/`, `src/sdk/`, `src/native_apps/` |

Everything else in the wallet (`wallet.cpp`, `wallet_ui.cpp`, `attest.cpp`, `rpc.cpp`, `pay_session.cpp`, the native runtime, the Lua bindings and the Lua apps) is specified and not written, so it has no tests yet.

## Commands

These work today from `docs/os/reference/code/`. They need a C99 compiler (`cc`). Binaries go to a directory outside the repository.

### Suite 1: `test_sol`

```bash
cd docs/os/reference/code
OUT=${OUT:-$(mktemp -d)}

cc -std=c99 -Wall -Wextra -Wpedantic -O2 -DSOL_HOST_SHA256 \
   sol_b58.c sol_curve.c sol_pda.c sol_tx.c sol_sha256.c test_sol.c \
   -o "$OUT/test_sol" && "$OUT/test_sol"
```

Expected output:

```
all sol tests passed
```

### Suite 2: `test_pay`

`test_pay.c` signs and verifies with [Monocypher](https://monocypher.org/), which is third-party code and is not in this repository. Get release 4.0.2 and put its four Ed25519 files in one directory:

```bash
MONO=${MONO:-$(mktemp -d)}
curl -sL https://monocypher.org/download/monocypher-4.0.2.tar.gz | tar -xz -C "$MONO" --strip-components=1
cp "$MONO/src/monocypher.c" "$MONO/src/monocypher.h" \
   "$MONO/src/optional/monocypher-ed25519.c" "$MONO/src/optional/monocypher-ed25519.h" "$MONO/"
ls "$MONO"/monocypher*
# monocypher-ed25519.c  monocypher-ed25519.h  monocypher.c  monocypher.h   (plus monocypher.pc from the archive)
```

The archive used when this page was written has SHA-256 `38d07179738c0c90677dba3ceb7a7b8496bcfea758ba1a53e803fed30ae0879c`. Monocypher is licensed BSD-2-Clause or CC0.

Then, still in `docs/os/reference/code/`:

```bash
cc -std=c99 -Wall -Wextra -Wpedantic -O2 -DSOL_HOST_SHA256 -I "$MONO" \
   pay_proto.c sol_b58.c sol_curve.c sol_pda.c sol_tx.c sol_sha256.c test_pay.c \
   "$MONO/monocypher.c" "$MONO/monocypher-ed25519.c" \
   -o "$OUT/test_pay" && "$OUT/test_pay"
```

Expected output:

```
all pay tests passed
```

### Suite 3: `test_attest`

`attest_parse.c` includes `attest.h`, which in the reference layout is under `sdk-headers/wallet/`. Still in `docs/os/reference/code/`:

```bash
cc -std=c99 -Wall -Wextra -Wpedantic -O2 -DSOL_HOST_SHA256 -I sdk-headers/wallet \
   attest_parse.c sol_b58.c test_attest.c \
   -o "$OUT/test_attest" && "$OUT/test_attest"
```

Expected output:

```
all attest tests passed
```

### Reading a failure

Each failed check prints `FAIL <file>:<line> <expression>`; the last line is `<n> FAILED`; the exit status is non-zero. A passing run prints only the one line shown above and exits 0.

### Syntax check of the API headers

The headers and the Tip Jar example are kept in `sdk-headers/` so that they stay compilable. This checks syntax only; nothing is linked or run.

```bash
for h in sdk-headers/app_host/badge_api.h sdk-headers/wallet/*.h; do
  cc  -std=c99 -Wall -Wextra -Wpedantic -fsyntax-only -x c "$h"
  c++ -std=c++17 -fno-exceptions -fno-rtti -Wall -Wextra -fsyntax-only -x c++ "$h"
done
c++ -std=c++17 -fno-exceptions -fno-rtti -Wall -Wextra -fsyntax-only sdk-headers/native_apps/registry.cpp
c++ -std=c++17 -fno-exceptions -fno-rtti -Wall -Wextra -fsyntax-only sdk-headers/native_apps/tipjar/tipjar.cpp
c++ -std=c++17 -fno-exceptions -fno-rtti -Wall -Wextra -Wno-pragma-once-outside-header -fsyntax-only \
    -x c++ sdk-headers/identity/identity_private.h
```

Expected output: none. The loop covers `badge_api.h` and the nine headers in `sdk-headers/wallet/`, each as C99 and as C++17. `tipjar.cpp` includes `badge_sdk.hpp`, which includes `badge_api.h`. `identity_private.h` is C++ only; the extra flag silences the warning clang gives when a header with `#pragma once` is compiled as the main file. The flags `-fno-exceptions -fno-rtti` are the strictest case; which flags the Arduino core really uses is not confirmed. [UNVERIFIED; the SDK rules avoid depending on exceptions and RTTI]

## Layout in the firmware tree

The firmware tree (`firmware/solana-os/` in this repository) does not exist yet. When it does, the files move as the table above says:

```
firmware/solana-os/
  src/wallet/
    sol.h sol_b58.c sol_curve.c sol_pda.c sol_tx.c sol_sha256.c
    pay_proto.h pay_proto.c
    attest_parse.c
    wallet.h pay_session.h attest.h wallet_crypto.h wallet_internal.h audit.h history.h
    vendor/monocypher.c vendor/monocypher.h
    vendor/monocypher-ed25519.c vendor/monocypher-ed25519.h
    vendor/jsmn.h
  test/host/
    CMakeLists.txt
    test_sol.c test_pay.c test_attest.c
    vectors.mjs vectors-to-h.mjs vectors.json vectors.h
```

- The same source files are compiled into the firmware and into the tests. There is no separate "test copy". In the firmware tree `attest.h` sits next to `attest_parse.c` in `src/wallet/`, so the `-I sdk-headers/wallet` of suite 3 is not needed there. The copy commands are in the [build and flash guide](../guides/build-and-flash.md#our-tree).
- `test/host/` is outside `src/`. `arduino-cli` compiles the sketch and everything under `src/` [UPSTREAM build, `firmware/solana-os/README.md:50-96`], so it never sees the tests. CMake is used only for the host tests; the firmware build stays an Arduino sketch. [OURS]
- On the badge `SOL_HOST_SHA256` is not defined and `sol_sha256()` calls mbedTLS. On the host the define selects the portable SHA-256 in the same file.
- Monocypher is vendored under `src/wallet/vendor/` because the firmware uses it too, for signature verification and for software-key signing. [OURS: upstream's TweetNaCl costs about a second per operation by upstream's own description]

## CMake

`firmware/solana-os/test/host/CMakeLists.txt`:

```cmake
# firmware/solana-os/test/host/CMakeLists.txt
cmake_minimum_required(VERSION 3.16)
project(badge_host_tests C)
set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(W ${CMAKE_CURRENT_SOURCE_DIR}/../../src/wallet)
add_compile_options(-Wall -Wextra -Wpedantic)
add_library(walletcore STATIC ${W}/sol_b58.c ${W}/sol_curve.c ${W}/sol_pda.c ${W}/sol_tx.c ${W}/sol_sha256.c
            ${W}/pay_proto.c ${W}/attest_parse.c ${W}/vendor/monocypher.c ${W}/vendor/monocypher-ed25519.c)
target_compile_definitions(walletcore PUBLIC SOL_HOST_SHA256)
target_include_directories(walletcore PUBLIC ${W} ${W}/vendor ${CMAKE_CURRENT_SOURCE_DIR})
enable_testing()
foreach(t test_sol test_pay test_attest)
  add_executable(${t} ${t}.c)
  target_link_libraries(${t} walletcore)
  add_test(NAME ${t} COMMAND ${t})
endforeach()
```

Run from the repository root:

```bash
cmake -S firmware/solana-os/test/host -B /tmp/badge-host \
  && cmake --build /tmp/badge-host \
  && ctest --test-dir /tmp/badge-host --output-on-failure
```

Expected: `test_sol`, `test_pay` and `test_attest` each report `Passed`, and the summary line reads `100% tests passed` (the exact wording of that line varies with the CMake version).

This CMake file was run on 2026-10-03 on a staged copy of that layout (the reference files and the wallet headers in `src/wallet/`, Monocypher 4.0.2 in `src/wallet/vendor/`, the tests in `test/host/`) with CMake 4.4.3 and Apple clang 17; all three tests passed. It has not been run in the real firmware tree.

Run it before every flash, together with the two `grep` rules for the key gate ([signing gate, Key gate](../wallet-core/signing-gate.md#key-gate)); the full pre-flash sequence is in the [build and flash guide](../guides/build-and-flash.md#pre-flash-checks).

## Vectors

`vectors.h` is generated, and committed so that the tests run without Node.

### Where they come from

`vectors.mjs` uses the same libraries the dashboard uses, from the dashboard's own `node_modules` (`@solana/kit` 5.5.1, `sas-lib` 1.0.10, `@solana-program/token` 0.9.0 per `dashboard/package.json`). It resolves them relative to the current directory, so it must be run from `dashboard/`.

It also contains an independent implementation of `find_program_address` and of the curve test in big-integer arithmetic. That implementation reproduces the library's credential, schema, attestation and token-account addresses (`independent_matches` and its siblings in `vectors.json` are all `true`), and it supplies the expected answers for the curve vectors. So the C code is checked against the libraries for addresses and messages, and against a second implementation for curve membership.

| Symbol in `vectors.h` | Content | Produced by |
|---|---|---|
| `CURVE_HEX[24]`, `CURVE_ON[24]`, `CURVE_N` | 24 SHA-256 digests (the candidates for bumps 255 to 248 of three derivations) and whether each is on the curve | the independent implementation |
| `V_PAYER`, `B58_PAYER` | stand-in Judge A key `4vJD…BW97`, bytes and base58 | `@solana/kit` |
| `V_DST_OWNER`, `B58_OWNER1` | stand-in merchant key `Gn2G…Ecxq` | `@solana/kit` |
| `V_MINT`, `B58_MINT` | `So11111111111111111111111111111111111111112`, used only as a 32-byte value | — |
| `V_SRC`, `V_DST`, `B58_ATA1` | associated token accounts of the payer and the merchant | `@solana-program/token` |
| `B58_CRED`, `B58_SCHEMA`, `V_ATT`, `B58_ATT` | SAS credential, schema and the attestation address for the merchant | `sas-lib` |
| `V_BH` | blockhash, 32 bytes of `0x01` | — |
| `V_LEGACY` (214 bytes), `V_V0` (216 bytes) | `transferChecked` messages for 1000 raw units, 2 decimals | `@solana/kit` + `@solana-program/token`, built the way the dashboard builds its attack transaction |
| `V_ACCT` (189 bytes) | an attestation account for the name `MHacks Merch`, expiry 1793664000 | assembled per the SAS layout and decoded back with `sas-lib` |
| `V_ALT_OWNER`, `V_ALT_MINT`, `V_ALT_SRC`, `V_ALT_DST`, `V_ALT_LEGACY` (214 bytes) | a second key set (owner `CiGJ…Qxj7`, mint `Fr76…sphm`) for which kit orders the two token accounts, and the mint and Token program, opposite to ascending raw bytes; `tx.legacy_alt_order` in the JSON | `@solana/kit` + `@solana-program/token`; the generator searches deterministic keys until kit's order differs |
| `V_CB_LEGACY` (258 bytes) | the vector transfer with a ComputeBudget `SetComputeUnitPrice` instruction in front, 6 account keys; `tx.legacy_compute_budget` in the JSON, which also holds it as base64 for acceptance test [T-F3](acceptance.md#t-f3) | `@solana/kit` + `@solana-program/token` |

`vectors.json` also holds a 50000-raw-unit message (`tx.legacy_50000`) and the wire forms; the C tests do not use them yet.

### How to regenerate

Needed only when the dashboard's libraries are upgraded or vectors are added. Requires Node 22.12 or later and `npm install` done in `dashboard/`.

Today, from the repository root:

```bash
cd dashboard
node ../docs/os/reference/code/vectors.mjs > ../docs/os/reference/code/vectors.json
cd ../docs/os/reference/code
node vectors-to-h.mjs
```

1. `vectors.mjs` prints the JSON on standard output; the redirect writes it into `reference/code/`.
2. `vectors-to-h.mjs` reads `vectors.json` from its own directory and writes `vectors.h` next to it. It needs no packages.

In the firmware tree the same steps use `../firmware/solana-os/test/host/` instead of `../docs/os/reference/code/`.

Checked on 2026-10-03 with Node 26.10: both steps reproduce the committed `vectors.json` and `vectors.h` byte for byte. After regenerating, run all three suites again.

## Coverage

What the three suites check today. Line numbers refer to the test files.

### `test_sol.c`

| Area | Checks | Lines |
|---|---|---|
| base58 | a kit address decodes to its bytes and encodes back; the all-zero key is 32 `1` characters both ways; characters outside the alphabet are rejected; the Token, ATA and SAS program-id constants encode to their published addresses | 12–19 |
| Curve membership | `sol_is_on_curve()` on 24 digests against the independent implementation | 21 |
| Addresses | `sol_ata()` for the merchant and the payer equals `@solana-program/token`'s result; `sol_sas_attestation_pda()` equals `sas-lib`'s, as bytes and as base58 | 23–29 |
| Decoder, accepted | a kit-built legacy message decodes with the right version, amount, decimals, payer, source, destination, mint and blockhash; a kit-built v0 message decodes | 31–34 |
| Builder, first vector | `sol_tx_build_transfer()` output equals kit's legacy message `V_LEGACY`, byte for byte | 36–37 |
| Second key set | kit's order differs from raw-byte order for `V_ALT_LEGACY`: `sol_ata()` reproduces its two token accounts; the decoder accepts kit's bytes and returns the right fields; kit's writable pair and its mint and Token program are not in ascending bytes; the builder's 214 bytes differ from kit's and decode to the same fields | 39–49 |
| Decoder, refused | 15 checks covering 14 kinds of rejection: truncated; trailing byte; two required signatures (`header`); instruction tag 3, plain `Transfer` (`ix_data`); program index pointing at the mint (`program`); a different program key, as Token-2022 would be (`program`); two instructions (`ix_count`); authority not the signer (`authority`); destination equal to source (`roles`); zero amount (`amount_zero`); version 1 (`version`); a v0 message with a lookup table (`lookups`); the 258-byte ComputeBudget message `V_CB_LEGACY` (`too_long`); six account keys (`accounts`); a `pay-req:` string | 51–65 |
| Amounts | `sol_format_amount()` for 10.00, 0.05, 500.00, the largest 64-bit value and 9 decimals; `sol_parse_amount()` for `10`, `0.05`, `12.5`, and refusal of too many decimals, an empty string and `1e3` | 67–75 |
| Attestation account offsets | the offsets of the discriminator, nonce, data length, name length and name in the 189-byte vector | 77 |

What the builder check does and does not show: `sol_tx_build_transfer` orders the two token accounts, and the mint and Token program, by raw bytes. The result is always a valid message that the decoder accepts and that decodes to the inputs. It equals `@solana/kit`'s bytes when kit's base58-text order coincides with raw-byte order (vector `V_LEGACY`) and differs otherwise (vector `V_ALT_LEGACY`: same transfer, different key order, both decode to the same fields). Order inside a role class has no effect on chain.

### `test_pay.c`

| Area | Checks | Lines |
|---|---|---|
| REQ | encode, `pay_peek()` and decode round trip; the signature over `pay_req_signed_bytes()` verifies with the payee key; flipping one bit of the amount makes verification fail | 16–23 |
| REQ field rules | name length 33, a control character, a non-zero padding byte and a short frame are each refused; a frame with version 2 is not recognised by `pay_peek()` | 24–28 |
| CHAL | encode and decode round trip | 29–30 |
| PROOF | encode and decode round trip; the signature over `pay_proof_signed_bytes()` verifies; the same signature does not verify for a different nonce | 31–35 |
| IAM | encode and decode round trip with a name | 36–37 |
| RCPT | encode and decode round trip; signature verifies | 38–39 |
| Size budgets | REQ ≤ 240, CHAL ≤ 64, PROOF ≤ 128, RCPT ≤ 240; every signed string ≤ 180 | 40–41 |
| Domain separation | none of the three signed strings is accepted by `sol_tx_decode_transfer()`; the REQ string starts with `0x70` and has `0x2d` as its fourth byte | 43–45 |

### `test_attest.c`

21 `CHECK` statements, at lines 15 to 48, against `V_ACCT` and mutated copies. The subject is the stand-in merchant key, the credential and schema are the vector's.

| Area | Checks | Lines |
|---|---|---|
| Setup and the vector | the credential and schema decode from base58; `V_ACCT` is 189 bytes; `attest_parse()` accepts it and returns `MHacks Merch` and expiry 1793664000 | 15–18 |
| Wrong identity fields | discriminator 0 and 1 (a credential or schema account); another key in the nonce; another credential; another schema | 21–24 |
| Wrong lengths inside the account | `data_len` that disagrees with the total length; `name_len` one below and one above `data_len - 4` | 25–26 |
| Name rule | a control byte, DEL and a non-ASCII byte; a leading and a trailing space; two consecutive spaces | 27–29 |
| Wrong total length | one byte short; one byte long; 104 bytes (too short to hold both length fields) | 30–33 |
| Wrong subject argument | the valid account, asked about another subject, is refused | 34 |
| Expiry | an expiry of 0 parses and is returned as 0 | 36–37 |
| Name length limits | a 1-character and a 32-character name are accepted; 33 characters are refused | 38–47 |

### Not covered

Found by reading the tests against the code. None of these is known to be wrong; they are untested.

- `pay_paid_encode()` / `pay_paid_decode()` and `pay_hello_encode()`.
- REQ refused for `amount == 0` and for `ttl_s == 0`; names with a leading or trailing space.
- Decoder error `ix_accounts`.
- `sol_b58_decode()` with a wrong output length; `sol_format_amount()` with a buffer that is too small; overflow in `sol_parse_amount()`.
- The owner check, the expiry decision and the claim comparison of the attestation check; they are in `attest.cpp`, which is not written.
- Any timing, stack or heap figure. Those need a badge ([measurements](measurements.md)).

## Tests to add

When the modules exist:

| Test | Input | Passes when |
|---|---|---|
| JSON extraction | captured responses of `getBalance`, `getTokenAccountBalance`, `getLatestBlockhash`, `sendTransaction` (success and error), `getSignatureStatuses`, `getAccountInfo` (account and `null`) | each value in [transaction building, Result extraction](../protocol/transaction-building.md#result-extraction) is extracted; malformed input gives `parse` |
| Severity policy | the function that maps identity, presence and red detail lines to green, amber or red, as a pure function | a table-driven test reproduces the table in [signing gate, Severity and gestures](../wallet-core/signing-gate.md#severity-and-gestures) |

Small additions that close the gaps listed above:

- A PAID round trip and a HELLO `pay_peek()` check in `test_pay.c`.
- The worked-example frames in [payment protocol](../protocol/payment-protocol.md#worked-example) as fixed byte vectors, so a change to the codec or the signed strings is caught.
- A builder round trip for the two orderings the vectors do not reach (source before destination with the mint before the Token program, and the reverse of both): build, decode, compare fields. `V_LEGACY` and `V_ALT_LEGACY` cover the other two.
- A decoder check for `ix_accounts`.

Lua apps have no host tests. Upstream ships a browser emulator of the Lua SDK (`app/src/lib/emulator/` in the upstream repository) [UPSTREAM]; it does not know the wallet modules. Stubbing them is optional and not planned.

## Requirements covered

Host tests are evidence for these requirements; they do not demonstrate them on hardware.

| Id | Requirement | What the host tests show |
|---|---|---|
| F3 | Only `transferChecked` is decoded; anything else is refused | the decoder accepts three kit-built messages (legacy, v0, and a legacy message in kit's other account order); 15 negative decoder checks covering 14 kinds of rejection, among them a kit-built message with a ComputeBudget instruction |
| F8 | Signed payment requests | REQ signature verifies; a tampered REQ does not |
| F9 | Nonce handshake | PROOF verifies for its nonce and not for another |
| F10 | Attestation check | the attestation address equals `sas-lib`'s; `attest_parse()` accepts the vector account and refuses every mutated copy |
| F19 | Co-signed receipts | RCPT round trip and signature |
| Goal 4 | Message signatures cannot be transaction signatures | domain-separation checks |

## Open items

| Item | Status | Fallback or how to resolve |
|---|---|---|
| The reference C compiles unchanged under the Arduino core | [UNVERIFIED] | it is plain C99; fix warnings as they appear |
| Run time and stack use of the derivation and of Ed25519 on the ESP32-S3 | [UNVERIFIED] | measure on a badge |
| Exact compiler flags of the installed Arduino core (`-std`, exceptions, RTTI) | [UNVERIFIED] | the SDK avoids depending on them |
| The CMake file has run only on a staged copy of the layout | to repeat | run it once the firmware tree exists |
| Untested code paths listed under [Not covered](#not-covered) | to add | see [Tests to add](#tests-to-add) |
