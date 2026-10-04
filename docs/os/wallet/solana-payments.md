# Solana payments: decoder, builder, tokens

The `solana` signing domain: which transaction messages the badge will even consider, how they are decoded, how apps build one, and how a payment is tied to the request it answers. Files: `src/vk/wallet/pure/sol.h`, `sol_b58.c`, `sol_sha256.c`, `sol_tx.c`, `vk_payment.{h,c}` (host-tested C99) and `src/vk/features/solana_pay/`.

The trust decision (who the recipient is, whether they are present) is in [checks.md](checks.md). This document is only about bytes.

## Starting point

`docs/os/reference/code/` holds a host-tested decoder and builder (`sol_tx.c`), base58 (`sol_b58.c`), SHA-256 (`sol_sha256.c`), their header (`sol.h`), a test (`test_sol.c`) and vectors generated with `@solana/kit` (`vectors.*`). Work package WP02 copies `sol.h`, `sol_b58.c`, `sol_sha256.c`, `sol_tx.c` to `src/vk/wallet/pure/` and `test_sol.c`, `vectors.*` to `test/host/`, then makes the changes below. `sol_curve.c` and `sol_pda.c` are not copied: the badge derives no addresses (the recipient's token account comes from the signed registry record, the badge's own from the RPC node).

The reference accepts exactly one instruction, five account keys, legacy or v0, up to 256 bytes. BadgeOS needs: legacy only, an optional Memo, up to 1232 bytes.

## Message format

A legacy Solana message, the bytes that get signed:

```
header            num_required_signatures u8, num_readonly_signed u8, num_readonly_unsigned u8
account keys      compact-u16 count, then count × 32 bytes
recent blockhash  32 bytes
instructions      compact-u16 count, then for each:
                    program_id_index u8
                    accounts: compact-u16 count, then count × u8 (indices into account keys)
                    data:     compact-u16 length, then that many bytes
```

compact-u16: 1 to 3 bytes, 7 bits each, low bits first, high bit set on every byte but the last. Only the shortest encoding of a value is accepted.

Fixed program ids (32 raw bytes each, constants in `sol_tx.c`):

| Program | Base58 |
|---|---|
| SPL Token | `TokenkegQfeZyiNwAJbNbGKPFXCWuBvf9Ss623VQ5DA` |
| Memo | `MemoSq4gqABAXKb96qnH8TysNcWxMyWCqXgDLGmfcHr` |

## Decoder rules

`sol_tx_decode_transfer` returns `SOL_TX_OK` only if **all** of these hold. Anything else is an error, and the wallet core then shows "CANNOT READ PAYMENT" and refuses to sign. It never signs what it cannot decode.

| # | Rule | Error |
|---|---|---|
| 1 | length 1..1232 | `SOL_TX_ERR_TOO_LONG`, `SOL_TX_ERR_TRUNCATED` |
| 2 | first byte has the high bit clear (not a versioned message) | `SOL_TX_ERR_VERSION` |
| 3 | header is `1, 0, 2` with 5 account keys, or `1, 0, 3` with 6 account keys | `SOL_TX_ERR_HEADER`, `SOL_TX_ERR_ACCOUNTS` |
| 4 | 1 instruction (5 keys) or 2 instructions (6 keys) | `SOL_TX_ERR_IX_COUNT` |
| 5 | exactly one instruction's program is SPL Token; with 6 keys the other's program is Memo | `SOL_TX_ERR_PROGRAM` |
| 6 | the Token instruction has exactly 4 account indices, all in range | `SOL_TX_ERR_IX_ACCOUNTS` |
| 7 | its data is exactly 10 bytes: `12`, amount u64 little-endian, decimals u8 (`12` = `TransferChecked`) | `SOL_TX_ERR_IX_DATA` |
| 8 | its accounts are `[source, mint, destination, authority]` and authority is index 0 | `SOL_TX_ERR_AUTHORITY` |
| 9 | source and destination are indices 1 and 2 in either order and differ; mint, Token program and Memo program are distinct indices ≥ 3 | `SOL_TX_ERR_ROLES` |
| 10 | amount is not zero | `SOL_TX_ERR_AMOUNT_ZERO` |
| 11 | the Memo instruction has zero accounts and 1 or more bytes of valid UTF-8 data | `SOL_TX_ERR_MEMO` |
| 12 | no bytes remain after the last instruction | `SOL_TX_ERR_TRAILING` |

How the table is applied (fixed by `sol_tx.c` and `test_sol`):

- Rules are checked in the order the bytes are parsed; rules 8, 9, 10 and 12 are checked after both instructions have been read. A message that ends early is `SOL_TX_ERR_TRUNCATED` wherever it ends.
- Rule 3: `SOL_TX_ERR_HEADER` when the three header bytes are not `1, 0, 2` or `1, 0, 3`; `SOL_TX_ERR_ACCOUNTS` when the key count is not the third header byte plus 3.
- A compact-u16 that is not the shortest encoding gives the error of the field it encodes: `ACCOUNTS` (key count), `IX_COUNT`, `IX_ACCOUNTS`, `IX_DATA` (the Token instruction's data length), `MEMO` (the Memo instruction's account count or data length).
- Rule 5: with 6 keys the two instructions may come in either order, Token then Memo or Memo then Token.

Why so strict: a second instruction can do anything (close the account, approve a delegate, pay a priority fee out of SOL). The badge is a payment key, not a general wallet, so everything it has no screen for is refused.

Changes to the reference `sol.h`:

```c
#define SOL_TX_MSG_MAX 1232

typedef enum {
  SOL_TX_OK = 0, SOL_TX_ERR_TOO_LONG, SOL_TX_ERR_TRUNCATED, SOL_TX_ERR_VERSION, SOL_TX_ERR_HEADER,
  SOL_TX_ERR_ACCOUNTS, SOL_TX_ERR_IX_COUNT, SOL_TX_ERR_PROGRAM, SOL_TX_ERR_IX_ACCOUNTS,
  SOL_TX_ERR_IX_DATA, SOL_TX_ERR_AUTHORITY, SOL_TX_ERR_ROLES, SOL_TX_ERR_AMOUNT_ZERO,
  SOL_TX_ERR_MEMO, SOL_TX_ERR_TRAILING
} sol_tx_err_t;                       /* SOL_TX_ERR_LOOKUPS is gone: every versioned message is ERR_VERSION */

typedef struct {
  uint8_t  fee_payer[32];             /* account 0; also the transfer authority */
  uint8_t  source[32];
  uint8_t  mint[32];
  uint8_t  destination[32];           /* the token account credited */
  uint8_t  blockhash[32];
  uint64_t amount;                    /* raw base units */
  uint8_t  decimals;
  const uint8_t *memo;                /* points into msg; NULL when there is no Memo */
  size_t   memo_len;
} sol_transfer_t;                     /* the `version` field is removed */

sol_tx_err_t sol_tx_decode_transfer(const uint8_t *msg, size_t len, sol_transfer_t *out);
const char *sol_tx_err_name(sol_tx_err_t e);

extern const uint8_t SOL_MEMO_PROGRAM_ID[32];
```

The decoder is structural only. Policy lives in the check chain: account 0 must be this badge, the mint must be in the token table, the decimals must match the table.

## Builder

Apps cannot pack a u64 (Lua integers are 32-bit), so the firmware builds the message.

```c
/* Legacy message for one TransferChecked, optionally followed by one Memo.
   Key order: payer; source and destination in ascending raw-byte order; then the readonly keys
   (mint, Token program, and the Memo program if memo_len > 0) in ascending raw-byte order.
   Returns the length, or 0 if cap is too small, source == destination, or memo is not valid UTF-8.
   Guarantee: sol_tx_decode_transfer() accepts the result and returns the same fields.
   To keep that guarantee it also returns 0 when amount == 0, when memo_len > 0 with a NULL memo, and
   when the message would exceed SOL_TX_MSG_MAX (a memo over 982 bytes). */
size_t sol_tx_build_transfer(const uint8_t payer[32], const uint8_t source[32],
                             const uint8_t destination[32], const uint8_t mint[32],
                             const uint8_t blockhash[32], uint64_t amount, uint8_t decimals,
                             const uint8_t *memo, size_t memo_len,
                             uint8_t *out, size_t cap);
```

Without a memo the result is 214 bytes. Byte-for-byte equality with `@solana/kit` is not guaranteed (kit orders keys by base58 text within a role); the order within a role has no effect on chain and the decoder reads accounts through the instruction's indices.

## Request memo

A payment that answers a request carries **one Memo instruction whose data is exactly the request's `req_id` as 16 lower-case hex characters**, with nothing before or after it (`00-Interfaces.md` §5: "In memos, `req_id` is written as 16 hex characters"). For the vector request that is `13cec0adb3e69e6e`. The transfer is then 214 + 32 (Memo program key) + 3 (program index, account count, length) + 16 = 265 bytes.

Why: RESULT frames are unsigned, so a payee cannot believe one. The memo puts the request id on chain inside the payer's signed transaction, which lets the payee check that a transaction it is told about pays *this* request ([Checking a received payment](#checking-a-received-payment)).

- **The payer's badge enforces it.** When `ctx.req` is supplied, the check chain requires the memo (check 12a, [checks](checks.md#the-check-chain)): a request payment with no memo, a free-text memo, another request's id or the id in upper case is red **WRONG MEMO** (`mismatch`). The decoder's rules are unchanged: still one transfer and at most one Memo with no accounts; everything it refused before it still refuses.
- **Without a request** a memo stays free text (a shop's item name) or absent; nothing changes for record-only payments.
- **Building it.** `wallet.build_transfer{..., req_id = <16 hex>}` writes the memo itself ([Lua API](../platform/lua-api.md#badgewallet-payments)), so an app never formats it. `req_id` together with a non-empty `memo` is `bad_arg`.
- **SE050.** A 265-byte transfer does not fit the SE050's 242-byte limit; on an SE050-keyed badge a request payment is refused with `too_long` ([signing](signing.md#key)). The SE050 is quarantined on every badge today.

```c
/* src/vk/wallet/pure/vk_payment.h */
#define VK_REQ_MEMO_LEN 16
void vk_req_memo(const uint8_t req_id[8], char out[VK_REQ_MEMO_LEN + 1]);   /* lower-case hex, NUL */
int  vk_memo_is_req(const uint8_t *memo, size_t len, const uint8_t req_id[8]);   /* 1: exactly that */
int  vk_req_id_parse(const char *text, size_t len, uint8_t out[8]);   /* 16 hex, either case; 0 = ok */
```

## Checking a received payment

The payee's half. Before the payee shows PAID it fetches the transaction the RESULT frame names (`getTransaction`) and asks the firmware whether those bytes are the payment it asked for. The check is a pure function:

```c
/* src/vk/wallet/pure/vk_payment.h */
int vk_wire_split(const uint8_t *tx, size_t len, const uint8_t **sig, const uint8_t **msg, size_t *msg_len);

typedef struct {
  const uint8_t *to;       /* 32: the token account that must be credited (required) */
  const uint8_t *mint;     /* 32 (required) */
  uint8_t decimals;
  uint64_t amount;         /* raw units, not 0 (required) */
  const uint8_t *req_id;   /* 8: the request the memo must name (required) */
  const uint8_t *payer;    /* 32 or NULL: the fee payer must be this key */
  const uint8_t *sig;      /* 64 or NULL: the transaction's signature must be this one */
  int (*verify)(const uint8_t *msg, size_t len, const uint8_t *sig, const uint8_t *pubkey);   /* NULL: not checked */
} vk_pay_expect_t;
typedef struct { sol_transfer_t transfer; uint8_t sig[64]; } vk_pay_seen_t;

vk_pay_err_t vk_payment_verify(const uint8_t *tx, size_t len, const vk_pay_expect_t *expect, vk_pay_seen_t *seen);
const char *vk_pay_err_name(vk_pay_err_t e);
vk_reason_t vk_pay_reason(vk_pay_err_t e);
```

`tx` is the **wire transaction**: compact-u16 signature count, the signatures, the message. Only a count of 1 (the single byte `0x01`) is accepted, because the badge's payments have one signer. The checks run in this order; the first difference is the answer:

| # | Check | `vk_pay_err_t` | Name | Reason |
|---|---|---|---|---|
| 1 | `expect` and its required fields are present, amount not 0 | `VK_PAY_ERR_ARG` | `arg` | `bad_arg` |
| 2 | a one-signature wire transaction with a non-empty message | `VK_PAY_ERR_WIRE` | `wire` | `undecodable` |
| 3 | the message passes the [decoder rules](#decoder-rules) (one transfer, at most one Memo) | `VK_PAY_ERR_SHAPE` | `shape` | `undecodable` |
| 4 | the signature verifies over the message with the fee payer's key (when `verify` is set) | `VK_PAY_ERR_SIGNATURE` | `signature` | `bad_proof` |
| 5 | the signature equals `expect.sig` (when set: the RESULT frame's `ref`) | `VK_PAY_ERR_SIG_MISMATCH` | `sig` | `mismatch` |
| 6 | the fee payer equals `expect.payer` (when set) | `VK_PAY_ERR_PAYER` | `payer` | `mismatch` |
| 7 | the mint and the decimals are the expected token's | `VK_PAY_ERR_MINT` | `mint` | `mismatch` |
| 8 | the destination is `expect.to` | `VK_PAY_ERR_RECIPIENT` | `recipient` | `mismatch` |
| 9 | the amount is `expect.amount` exactly | `VK_PAY_ERR_AMOUNT` | `amount` | `mismatch` |
| 10 | the memo is exactly `vk_req_memo(expect.req_id)` | `VK_PAY_ERR_MEMO` | `memo` | `mismatch` |

What it does not do: it reads bytes only. Whether the transaction landed and succeeded is in the RPC reply's `meta.err` (null when it succeeded), which the Lua library checks; a lying RPC node can claim anything about the chain, and this check does not change that. What it does stop is the cheap attack: a forged RESULT frame naming any confirmed transaction (someone else's, or a transfer of 0.01 to the payee) no longer turns the payee's screen to PAID.

Lua: `wallet.verify_payment(tx, expected)` and `wallet.record_received(tx, expected)` run it with `verify` set (one Ed25519 verification, about 18 ms with Monocypher) ([Lua API](../platform/lua-api.md#badgewallet-received-payments)).

## Amount helpers

Amount helpers already in the reference, unchanged:

```c
size_t sol_format_amount(uint64_t raw, uint8_t decimals, char *out, size_t cap);   /* 1250, 2 -> "12.50" */
int    sol_parse_amount(const char *text, uint8_t decimals, uint64_t *raw);        /* "12.5", 2 -> 1250; 0 on success */
```

## Token table

Which tokens the badge will pay with. Provisioned, never compiled in ([config](../platform/config.md#token-table)).

```c
/* src/vk/wallet/pure/vk_checks.h */
typedef struct {
  uint8_t  mint[32];
  uint8_t  decimals;
  char     symbol[5];      /* NUL-terminated, 1..4 chars of [A-Z0-9]: it must fit the 4-byte currency field of a REQ frame */
  uint64_t cap;            /* raw units; above this SELECT becomes a hold. 0 = no cap */
  uint64_t max;            /* raw units; above this the payment is blocked. 0 = no max */
} vk_token_t;
#define VK_MAX_TOKENS 3   /* keeps the provisioning line under the 256-byte serial receive buffer */
```

A message whose mint is not in the table, or whose decimals differ from the table's, is "UNKNOWN TOKEN" (`undecodable`). The amount on the screen is always `sol_format_amount(amount, table.decimals)` followed by `table.symbol`: from the decoded bytes and the provisioned table, never from the app.

Adding a token is one more entry in the provisioned `tokens` value. No code changes.

## The feature folder

`src/vk/features/solana_pay/`:

| File | Contents |
|---|---|
| `domain_solana.cpp` | `VK_SIGN_DOMAIN(solana, "solana", "", true, "sign", 1232, decodeSolana, nullptr)`. `decodeSolana` verifies the record, raises the clock floor, calls `vk_check_solana` ([checks](checks.md#the-check-chain)) and turns the verdict into an `ApprovalRequest` ([checks](checks.md#verdict-to-screen)) |
| `lua_solana.cpp` | Lua functions `wallet.begin_solana`, `wallet.build_transfer`, `wallet.wire_tx`, `wallet.check_record` ([Lua API](../platform/lua-api.md#badgewallet-payments)) |

## Tests

Host suite `test_sol` (in `test/host/`), extending the reference test:

- everything the reference already checks that still applies: base58, legacy vector decode, builder round trip, amount format and parse;
- a 5-key message and a 6-key message with a Memo both decode, and the memo bytes are returned;
- one negative vector per rule in the table above, including the five named in the product spec: a second non-memo instruction, a Memo with an account, two signers, a versioned (v0) message, trailing bytes;
- compact-u16: a two-byte memo length (memo of 200 bytes) decodes; a non-minimal encoding is refused;
- builder output with and without a memo is accepted by the decoder for 100 random key sets.

Host suite `test_payment`: the request memo (encoding, exact match, hex parsing), the wire split (one signature only, the shortest count), `vk_payment_verify` accepting the honest payment and refusing each row of the table above with its own error (wrong signature, someone else's signature, another payer, mint, decimals, token account, owner address, amount ±1, no memo, another request's memo, upper case, free text), and the daily-limit parser and window ([checks](checks.md#daily-limit)).

`vectors.mjs` is extended to emit the Memo vector with `@solana/kit`, building the Memo instruction by hand (program address, no accounts, UTF-8 data) so no new npm package is needed. Run it from `dashboard/` so it uses the dashboard's installed packages.
