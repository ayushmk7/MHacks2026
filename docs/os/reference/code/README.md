# Reference code

The starting code for the firmware's pure Solana modules, kept unchanged as it was when it was first host-tested. The firmware's own copies in `os/src/vk/wallet/pure/` have since been changed as [solana-payments](../../wallet/solana-payments.md) says; read those for what runs on the badge, and this folder for the original.

| File | What it is |
|---|---|
| `sol.h` | the shared header |
| `sol_b58.c` | base58 |
| `sol_sha256.c` | SHA-256 |
| `sol_tx.c` | the Solana message decoder and transfer builder |
| `sol_curve.c`, `sol_pda.c` | curve membership and program-derived addresses (associated token accounts); not in the firmware |
| `test_sol.c` | the host test |
| `vectors.mjs`, `vectors-to-h.mjs`, `vectors.json`, `vectors.h` | test vectors made with the dashboard's Solana libraries (run `vectors.mjs` from `dashboard/`), and the C header made from them. The firmware's tests use their own, newer copy in `os/test/host/` |

Build and run the test from this folder:

```bash
cc -std=c99 -Wall -Wextra -Wpedantic -O2 -DSOL_HOST_SHA256 \
   sol_b58.c sol_curve.c sol_pda.c sol_tx.c sol_sha256.c test_sol.c -o /tmp/test_sol && /tmp/test_sol
# all sol tests passed
```
