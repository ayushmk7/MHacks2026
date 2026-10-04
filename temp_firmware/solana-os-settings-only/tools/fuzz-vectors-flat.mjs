// Flattens harness/fuzz-vectors.json for tools/wallet_decode_test.cpp:
//   line 1: <payer base58> <mint base58>
//   then:   <id> <fn> <expect> <first argument as hex, or "-">
import { readFileSync } from 'node:fs';

const set = JSON.parse(readFileSync(process.argv[2], 'utf8'));
console.log(set.payer, set.mint);
for (const c of set.cases) {
  const a = c.args[0];
  console.log(c.id, c.fn, c.expect, a?.t === 'b' && a.hex ? a.hex : '-');
}
