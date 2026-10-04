// Feed: stat tiles, the block-chain strip (one payment = one block), block detail, Capital One settlements, bank-rail approvals,
// minute bars, Tiger Data card, ledger. Settlements join the chain rows by txSig.
import { useLayoutEffect, useRef, useState } from 'react';
import { useApi, useData, useGaps, useMode, useStatus } from '../data.jsx';
import { Amount, Bars, Empty, ExplorerLink, GapCard, Page, Pubkey, Skeleton, StatTile, StatusDot, ago, fmt, notBuilt, short, usd, useCopy, useNow } from '../ui.jsx';

const GLYPH = { verified: '✓', unverified: '?', revoked: '⊘' };
const PAGE = 40, RAIL_LEN = 30;
const nameOf = party => party.name || party.label || short(party.pubkey);
const kindOf = p => (p.attackId ? 'attack' : p.payee.status);
const bytes = b => (b == null ? '—' : b >= 1048576 ? `${(b / 1048576).toFixed(1)} MB` : `${Math.round(b / 1024)} kB`);
const clock = iso => new Date(iso).toLocaleTimeString('en-GB');

function Block({ p, now, selected, anim, onSelect }) {
  const kind = kindOf(p), offChain = p.source !== 'chain';
  const digits = fmt(p.amount, p.decimals).length; // long amounts step the type down so they never clip
  return (
    <div className={`block-wrap${anim === 'new' ? ' is-new' : anim === 'quiet' ? ' is-quiet' : ''}`} style={typeof anim === 'number' ? { '--i': anim } : undefined}>
      <button type="button" aria-pressed={selected} onClick={() => onSelect(selected ? null : p)}
        className={`block block--${kind}${offChain ? ` block--offchain block--${p.source}` : ''}${selected ? ' is-sel' : ''}`}
        aria-label={`Slot ${p.slot}: ${p.amount} ${p.symbol} to ${nameOf(p.payee)}, ${kind === 'attack' ? 'tampered transfer, signed' : p.payee.status}`}>
        <span className="b-top">
          <span className="b-glyph" aria-hidden="true">{kind === 'attack' ? '!' : GLYPH[kind]}</span>
          <span className="b-slot">{p.slot}</span>
        </span>
        <span className={`b-amt${digits > 8 ? ' b-amt--xs' : digits > 4 ? ' b-amt--sm' : ''}`}><Amount raw={p.amountRaw} decimals={p.decimals} symbol={p.symbol} /></span>
        <span className="b-who">{kind === 'attack' ? 'TAMPERED — signed' : <StatusDot status={p.payee.status} label={nameOf(p.payee)} />}</span>
        <span className="b-foot">
          <span>{ago(p.blockTime, now)} ago</span>
          {offChain && <span className="b-tag" title={`${p.source} data, not on devnet`}>~{p.source}</span>}
        </span>
      </button>
      <i className="link" aria-hidden="true" />
    </div>
  );
}

function Chain({ rows, sel, onSelect, now, listening }) {
  const rail = useRef(null), head = useRef(rows[0]?.signature);
  const [scrolled, setScrolled] = useState(false), [unseen, setUnseen] = useState(0);
  const blocks = rows.slice(0, RAIL_LEN);
  // How each block entered: a number = present at mount (staggered rise), 'new' = slid in at the head,
  // 'quiet' = arrived while the reader was scrolled away. ponytail: this map only grows; fine for one session.
  const seen = useRef(null);
  if (!seen.current) seen.current = new Map(rows.map((p, i) => [p.signature, i]));
  for (const p of blocks) if (!seen.current.has(p.signature)) seen.current.set(p.signature, scrolled ? 'quiet' : 'new');

  // No yank: if the reader has scrolled right, hold their blocks still and raise the "new" chip instead.
  useLayoutEffect(() => {
    const added = blocks.findIndex(p => p.signature === head.current);
    head.current = blocks[0]?.signature;
    if (!scrolled || added <= 0) return;
    const el = rail.current;
    el.scrollLeft += (el.querySelector('.block-wrap')?.offsetWidth ?? 0) * added;
    setUnseen(n => n + added);
  }, [blocks[0]?.signature]); // eslint-disable-line react-hooks/exhaustive-deps

  const jump = () => rail.current.scrollTo({ left: 0, behavior: matchMedia('(prefers-reduced-motion: reduce)').matches ? 'auto' : 'smooth' });
  return (
    <div className={`rail${blocks.some(p => p.signature === sel?.signature) ? ' has-sel' : ''}`} ref={rail} tabIndex={0} role="group" aria-label="Payment chain, newest first. Scroll right for older blocks."
      onScroll={e => { const s = e.currentTarget.scrollLeft > 40; setScrolled(s); if (!s) setUnseen(0); }}
      onClick={e => !e.target.closest('button') && onSelect(null)}
      // Keyboard focus only: the browser counts a block under the sticky NEXT slot as visible, so pull it clear (scroll-padding in the CSS).
      onFocus={e => e.target.matches('.block:focus-visible') && e.target.scrollIntoView({ block: 'nearest', inline: 'nearest' })}>
      <div className="rail-head">
        <div className="block next">
          <span className="b-top"><span className="next-label">NEXT</span></span>
          <span className="next-bars" aria-hidden="true"><i /><i /><i /></span>
          {unseen > 0 ? <button type="button" className="btn btn--small btn--primary newchip" onClick={jump}>← {unseen} new</button>
            : <span className="next-state"><i className={`dot${listening ? ' pulse' : ''}`} aria-hidden="true" />{listening ? 'listening' : 'idle'}</span>}
        </div>
      </div>
      {blocks.map(p => (
        <Block key={p.signature} p={p} now={now} anim={seen.current.get(p.signature)} selected={sel?.signature === p.signature} onSelect={onSelect} />
      ))}
      <div className="rail-end">older blocks<br />in the ledger ↓</div>
    </div>
  );
}

function AttackLine({ p }) {
  const { data } = useData('attacks');
  const a = data?.attempts.find(x => x.id === p.attackId);
  return (
    <div className="note note--bad" role="alert">
      <b>TAMPERED — signed.</b>{' '}
      {a ? <>The checkout claimed <b>{fmt(a.displayAmount)} {a.symbol}</b>. The transfer that was signed moved <b>{fmt(a.actualAmount)} {a.symbol}</b>.</>
        : <>This transfer matches attack attempt <span className="mono">{short(p.attackId, 6)}</span>: the amount on chain is not the amount the checkout showed.</>}
    </div>
  );
}

// Capital One settlement of a payment (payee settles to the bank): settled $X / settling / failed / skipped. HACK is 1:1 with dollars.
const SETTLE = { settled: 'chip--ok', pending: 'chip--warn', failed: 'chip--bad', skipped: '' };
const settleText = s => (s.status === 'settled' ? `settled ${usd(Math.round(s.amount * 100))}` : s.status === 'pending' ? 'settling'
  : s.status === 'skipped' ? (s.reason === 'amount_not_whole_dollars' ? 'skipped · fractional' : 'skipped') : s.status === 'failed' ? 'settle failed' : s.status);
function SettleChip({ s }) {
  if (!s) return null;
  const title = `Capital One settlement: ${s.status}${s.reason ? ` (${settleReason(s.reason)})` : ''}${s.attempts ? `, ${s.attempts} attempt${s.attempts === 1 ? '' : 's'}` : ''}`;
  return <span className={`chip ${SETTLE[s.status] ?? ''}`} title={title}>{settleText(s)}</span>;
}

function Detail({ p, now, onClose, settlement }) {
  const [copied, copy] = useCopy();
  if (!p) return <div className="detail detail--empty"><span aria-hidden="true">↑</span> Select a block to inspect its transaction.</div>;
  const kind = kindOf(p);
  return (
    <div className={`card detail block--${kind}`}>
      <div className="detail-top">
        <span className="b-glyph" aria-hidden="true">{kind === 'attack' ? '!' : GLYPH[kind]}</span>
        <span className="detail-amt"><Amount raw={p.amountRaw} decimals={p.decimals} symbol={p.symbol} /></span>
        <span className="detail-flow">{nameOf(p.payer)} <span aria-hidden="true">→</span> {nameOf(p.payee)}</span>
        <StatusDot status={p.payee.status} />
        <span className="grow" />
        <ExplorerLink sig={p.signature}>Explorer</ExplorerLink>
        <button type="button" className="btn btn--small" onClick={onClose}>Close</button>
      </div>
      {p.attackId && <AttackLine p={p} />}
      <div>
        <div className="label">Signature</div>
        <div className="sig">
          <span className="hash">{p.signature}</span>
          <button type="button" className="btn btn--small" onClick={() => copy(p.signature)}>{copied ? 'Copied' : 'Copy'}</button>
        </div>
        {p.source !== 'chain' && <div className="muted small">~{p.source} block: this signature was never sent to devnet, so the explorer will not find it.</div>}
      </div>
      <dl className="kv">
        <div><dt>Payer</dt><dd>{p.payer.label ?? 'Unknown wallet'}<br /><Pubkey value={p.payer.pubkey} link /></dd></div>
        <div><dt>Payee</dt><dd>{p.payee.name ?? p.payee.label ?? 'Unknown wallet'}{p.payee.name && p.payee.label ? ` · ${p.payee.label}` : ''}<br /><Pubkey value={p.payee.pubkey} link /></dd></div>
        <div><dt>Amount</dt><dd className="num">{fmt(p.amount, p.decimals)} {p.symbol}<br /><span className="muted">{p.amountRaw} raw · {p.decimals} decimals</span></dd></div>
        <div><dt>Slot</dt><dd className="num">{p.slot}</dd></div>
        <div><dt>Block time</dt><dd className="num">{clock(p.blockTime)}<br /><span className="muted">{ago(p.blockTime, now)} ago</span></dd></div>
        <div><dt>Ingest lag</dt><dd className="num">{p.lagMs == null ? '—' : `${fmt(p.lagMs)} ms`}<br /><span className="muted">chain → database</span></dd></div>
        <div><dt>Source</dt><dd className="num">{p.source}</dd></div>
        {settlement && <div><dt>Capital One</dt><dd><SettleChip s={settlement} />
          {settlement.nessieDepositId && <><br /><span className="hash small" title={`Nessie deposit ${settlement.nessieDepositId}`}>deposit {short(settlement.nessieDepositId, 4)}</span></>}
          {settlement.reason && <><br /><span className="muted small">{settleReason(settlement.reason)}</span></>}</dd></div>}
      </dl>
    </div>
  );
}

// Why a payment was refused, in words a judge reads from 2 m. Server check-chain reasons (P2-U 3.1) and badge-side ones (00 4).
const REASON = {
  replay: 'Replay: this approval was already used', bad_sig: 'Bad signature on the approval', revoked: 'Payee attestation revoked',
  expired: 'Payee attestation expired', payee_mismatch: 'Payee does not match the attested record', bad_proof: 'Presence proof failed (payee not in the room)',
  amount_not_whole_dollars: 'Amount is not whole dollars', not_enrolled: 'Payer is not enrolled at the bank', stale: 'Approval too old (stale)',
  unverified: 'Payee not verified', mismatch: 'Amount or payee mismatch', cancelled: 'Cancelled on the badge', timeout: 'Timed out on the badge',
  undecodable: 'Request could not be decoded', unverified_hop: 'Unverified relay on the route', route_dropped: 'Route dropped by a relay',
  nessie_error: 'Bank call failed', insufficient_funds: 'Insufficient funds',
};
const SETTLE_REASON = { amount_not_whole_dollars: 'Not whole dollars: Nessie only takes whole dollars', nessie_error: 'Bank deposit failed' };
const reasonText = r => (r ? REASON[r] ?? r.replace(/_/g, ' ') : null);
const settleReason = r => (r ? SETTLE_REASON[r] ?? reasonText(r) : null);
const RAIL = { nessie: 'NESSIE', solana: 'SOLANA' };
// HACK has 2 decimals, so on the Solana rail amountCents is raw base units: 1200 -> 12 HACK.
const money = (a, symbol) => (a.rail === 'solana' ? `${fmt(a.amountCents / 100)} ${symbol}` : usd(a.amountCents));
// nessieIds: the contract names it but not its shape; take an array, an object of ids or one string.
const idsOf = v => (v == null ? [] : Array.isArray(v) ? v : typeof v === 'object' ? Object.values(v) : [v]).filter(Boolean).map(String);

function Approvals({ symbol }) {
  const ap = useData('approvals', { limit: 50 }), badges = useData('badges');
  const rows = ap.data?.approvals ?? [];
  const labelOf = pk => badges.data?.badges.find(b => b.pubkey === pk)?.label ?? short(pk);
  const blocked = rows.filter(a => a.status === 'blocked' || a.status === 'failed').length;
  // An older backend has no /api/approvals yet: say so quietly instead of raising an error card.
  if (notBuilt(ap.error) && !ap.data) return (
    <section className="card" aria-label="Bank rail approvals">
      <div className="card-head"><h2>Approvals</h2><span className="chip">bank rail</span></div>
      <p className="muted">This backend has no <code>/api/approvals</code> yet. Bank-rail payments and blocked attempts appear here once it ships.</p>
    </section>
  );
  return (
    <section className="card" aria-label="Bank rail approvals">
      <div className="card-head">
        <h2>Approvals</h2>
        <span className="muted">{ap.loading ? '' : `${fmt(rows.length - blocked)} through · ${fmt(blocked)} blocked`}</span>
      </div>
      {ap.loading ? <Skeleton variant="row" lines={5} />
        : !rows.length ? (ap.error ? <Empty icon="×" title={ap.error.code === 'offline' ? 'Backend offline' : 'Approvals unavailable'} hint={ap.error.message} />
          : <Empty icon="∅" title="No approvals yet" hint="Every bank-rail payment, and every attempt the backend or a badge refused, lands here." />)
        : (
          <div className="table-wrap">
            <table className="table">
              <thead><tr><th>Time</th><th>Rail</th><th>From</th><th>To</th><th>Status</th><th className="r">Amount</th><th>Links</th></tr></thead>
              <tbody>
                {rows.map(a => {
                  const ids = idsOf(a.nessieIds), memo = a.links?.memo;
                  return (
                    <tr key={a.id}>
                      <td className="num" title={a.time}>{clock(a.time)}</td>
                      <td><span className="chip">{RAIL[a.rail] ?? a.rail ?? '—'}</span></td>
                      <td>{labelOf(a.payer?.pubkey)}</td>
                      <td>{a.payee?.name ?? <span className="muted">{labelOf(a.payee?.pubkey)} · unverified</span>}</td>
                      <td>
                        <StatusDot status={a.status} />
                        {a.source === 'badge_report' && <> <span className="chip" title="Refused on the badge and reported by it (unauthenticated)">reported by badge</span></>}
                        {a.reason && <><br /><span className={`small ${a.status === 'blocked' || a.status === 'failed' ? 't-bad' : 'muted'}`} title={a.reason}>{reasonText(a.reason)}</span></>}
                      </td>
                      <td className="r num">{money(a, symbol)}</td>
                      <td>
                        {memo ? <a className="ext-link" href={memo} target="_blank" rel="noreferrer" title={a.memoSig ?? memo}>memo<span aria-hidden="true"> ↗</span></a>
                          : a.memoSig ? <ExplorerLink sig={a.memoSig}>memo</ExplorerLink> : null}
                        {ids.map((id, i) => <span key={id} className="hash small" title={`Nessie id ${id}`}>{i || memo || a.memoSig ? ' · ' : ''}nessie {short(id, 4)}</span>)}
                        {!memo && !a.memoSig && !ids.length && <span className="hash">—</span>}
                      </td>
                    </tr>
                  );
                })}
              </tbody>
            </table>
          </div>
        )}
    </section>
  );
}

function Settlements({ st }) {
  const rows = st.data?.settlements ?? [];
  const count = k => rows.filter(s => s.status === k).length;
  if (notBuilt(st.error) && !st.data) return (
    <section className="card" aria-label="Capital One settlements">
      <div className="card-head"><h2>Capital One settlements</h2></div>
      <p className="muted">This backend has no <code>/api/settlements</code> yet. Payments to payees that settle to Capital One show their dollar deposit here once it ships.</p>
    </section>
  );
  return (
    <section className="card" aria-label="Capital One settlements">
      <div className="card-head">
        <h2>Capital One settlements</h2>
        <span className="muted">{st.loading ? '' : `${fmt(count('settled'))} settled · ${fmt(count('pending'))} pending · ${fmt(count('failed'))} failed · ${fmt(count('skipped'))} skipped`}</span>
      </div>
      {st.loading ? <Skeleton variant="row" lines={4} />
        : !rows.length ? (st.error ? <Empty icon="×" title={st.error.code === 'offline' ? 'Backend offline' : 'Settlements unavailable'} hint={st.error.message} />
          : <Empty icon="∅" title="No settlements yet" hint="A payment to a payee that settles to Capital One lands in the bank's settlement wallet, then is deposited as dollars." />)
        : (
          <div className="table-wrap">
            <table className="table">
              <thead><tr><th>Time</th><th>Payee</th><th>Status</th><th className="r">Amount</th><th>Nessie deposit</th><th className="r">Attempts</th><th>Payment</th></tr></thead>
              <tbody>
                {rows.slice(0, 12).map(s => (
                  <tr key={s.txSig}>
                    <td className="num" title={s.time}>{clock(s.time)}</td>
                    <td>{nameOf(s.payee ?? {})}</td>
                    <td><SettleChip s={s} />{s.reason && <><br /><span className={`small ${s.status === 'failed' ? 't-bad' : 'muted'}`} title={s.reason}>{settleReason(s.reason)}</span></>}</td>
                    <td className="r num">{usd(Math.round((s.amount ?? 0) * 100))}</td>
                    <td className="hash">{s.nessieDepositId ? <span title={s.nessieDepositId}>{short(s.nessieDepositId, 6)}</span> : '—'}</td>
                    <td className="r num">{s.attempts ?? '—'}</td>
                    <td><ExplorerLink sig={s.txSig} /></td>
                  </tr>
                ))}
              </tbody>
            </table>
          </div>
        )}
    </section>
  );
}

function TigerCard({ db }) {
  const now = useNow(5000), d = db.data;
  return (
    <div className="card tiger">
      <div className="card-head">
        <span className="label">Powered by Tiger Data</span>
        {d && <span className="chip">TimescaleDB {d.timescaledb ?? '—'}</span>}
      </div>
      {db.loading ? <Skeleton variant="text" lines={5} />
        : !d ? <Empty icon="∅" title="Stats unavailable" hint={db.error?.message} />
        : <>
          <div className="tiger-ratio">
            <span className="num t-ok">{d.payments.compressionRatio == null ? '—' : `${(d.payments.compressionRatio * 100).toFixed(1)}%`}</span>
            <span>columnstore compression, measured<br /><span className="muted">
              {d.payments.compressionRatio == null ? 'no chunk converted yet' : `${bytes(d.payments.bytesBefore)} → ${bytes(d.payments.bytesAfter)}`}</span></span>
          </div>
          <dl className="kv kv--tight">
            <div><dt>Hypertable rows</dt><dd className="num">{fmt(d.payments.rows)}</dd></div>
            <div><dt>Chunks in columnstore</dt><dd className="num">{fmt(d.payments.columnstoreChunks)} / {fmt(d.payments.chunks)}</dd></div>
            <div><dt>Query time</dt><dd className="num">feed {fmt(d.queryMs?.feed)} ms · series {fmt(d.queryMs?.series)} ms</dd></div>
          </dl>
          <ul className="caggs" aria-label="Continuous aggregates">
            {d.caggs.map(c => (
              <li key={c.name}>
                <span className="mono">{c.name}</span>
                <span className={`chip ${c.realtime ? 'chip--ok' : ''}`}>{c.realtime ? 'real-time' : 'materialized'}</span>
                <span className="muted">{c.lastRefresh ? `refreshed ${ago(c.lastRefresh, now)} ago` : 'not refreshed yet'}</span>
              </li>
            ))}
          </ul>
        </>}
    </div>
  );
}

export default function Feed() {
  const now = useNow();
  const { mode } = useMode(), { status, error: down } = useStatus(), { get } = useApi();
  const pay = useData('payments'), stats = useData('stats'), db = useData('stats/db'), st = useData('settlements', { limit: 200 });
  const settled = new Map((st.data?.settlements ?? []).map(s => [s.txSig, s])); // ponytail: only the latest 200 settlements join the ledger
  const [sel, setSel] = useState(null), [older, setOlder] = useState([]), [more, setMore] = useState('idle'); // idle | loading | end | error
  const detail = useRef(null);

  const rows = pay.data?.payments ?? [], s = stats.data, symbol = status?.token?.symbol ?? 'HACK';
  const mintGap = useGaps('feed').find(g => g.code === 'hack_mint_missing');
  // ponytail: if the base list is refetched after new blocks arrived, the rows between it and `older` can go missing until reload.
  const ledger = [...rows, ...older.filter(o => !rows.some(r => r.signature === o.signature))];
  const listening = mode === 'demo' || (!down && status?.ingest?.state === 'live');

  async function loadOlder() {
    setMore('loading');
    try {
      // `before` is strict and chain block times are whole seconds, so two transfers often share one: ask from 1 ms later and
      // drop what is already shown, or the twin of the last row is skipped. ponytail: 40 rows in one millisecond would never page past it.
      const page = (await get('payments', { limit: PAGE, before: new Date(Date.parse(ledger.at(-1).blockTime) + 1).toISOString() })).payments;
      setOlder(o => [...o, ...page.filter(p => !o.some(x => x.signature === p.signature))]);
      setMore(page.length < PAGE ? 'end' : 'idle');
    } catch { setMore('error'); }
  }
  const inspect = p => { setSel(p); detail.current?.scrollIntoView({ block: 'nearest' }); };

  return (
    <Page title="Payments" subtitle={`Every ${symbol} transfer is a block. Capital One settlements, bank-rail approvals and blocked attempts are listed under the chain.`}>
      <section className="tiles" aria-label="Last 24 hours">
        {stats.loading ? [0, 1, 2, 3].map(i => <Skeleton key={i} variant="card" />) : <>
          <StatTile label="Payments · 24h" value={fmt(s?.payments)} sub={!s ? 'unavailable' : s.seedRows ? `includes ${fmt(s.seedRows)} seeded rows` : 'confirmed on devnet'} />
          <StatTile label={`${symbol} volume · 24h`} value={fmt(s?.volume)} sub={s ? 'from the 1-minute aggregate' : 'unavailable'} />
          <StatTile label="Verified share" value={s ? `${Math.round(s.verifiedShare * 100)}%` : '—'} tone={s ? 'ok' : undefined} sub={s ? 'of payments went to a verified payee' : 'unavailable'} />
          <StatTile label="Ingest lag · p95" value={s?.lagMs?.p95 == null ? '—' : `${(s.lagMs.p95 / 1000).toFixed(1)}s`}
            tone={s?.lagMs?.p95 == null ? undefined : s.lagMs.p95 <= 3000 ? 'ok' : 'warn'}
            sub={!s ? 'unavailable' : s.lagMs?.p50 == null ? 'no live payments in the last hour' : `p50 ${(s.lagMs.p50 / 1000).toFixed(1)}s · target under 3s`} />
        </>}
      </section>

      <section className="card chain" aria-label="Block chain">
        <div className="card-head">
          <h2>Chain</h2>
          <span className="legend" aria-label="Block states">
            <span className="t-ok"><span className="b-glyph">✓</span>verified</span>
            <span className="t-warn"><span className="b-glyph">?</span>unverified</span>
            <span className="t-bad"><span className="b-glyph">⊘</span>revoked</span>
            <span className="t-bad"><span className="b-glyph">!</span>tampered</span>
          </span>
        </div>
        {/* BADGE-GAP(hack-mint): no mint, no payments to listen for. The server's gap card names the fix; never fake blocks in LIVE. */}
        {mintGap ? <div className="chain-msg"><GapCard gap={mintGap} /></div>
          : pay.loading ? <div className="rail" aria-busy="true">{Array.from({ length: 9 }, (_, i) => <Skeleton key={i} w="var(--bw)" h="var(--bh)" />)}</div>
          : rows.length ? <Chain rows={rows} sel={sel} onSelect={setSel} now={now} listening={listening} />
          : <div className="chain-idle">
            <div className="rail rail--idle" aria-hidden="true">{Array.from({ length: 9 }, (_, i) => <div key={i} className="block block--void" />)}</div>
            {pay.error ? <Empty icon="×" title={pay.error.code === 'offline' ? 'Backend offline' : 'Feed unavailable'} hint={pay.error.message} />
              : <Empty icon="∅" title="No payments yet" hint="Run `npm run pay`, or switch to DEMO." />}
          </div>}
      </section>

      <section ref={detail} className="detail-slot" aria-label="Selected block" aria-live="polite">
        {(pay.loading || rows.length > 0) && <Detail p={sel} now={now} onClose={() => setSel(null)} settlement={sel && settled.get(sel.signature)} />}
      </section>

      <Settlements st={st} />

      <Approvals symbol={symbol} />

      <section className="split">
        <div className="card">
          <div className="card-head">
            <span className="label">Payments per minute · last hour</span>
            <span className="legend"><span><i className="dot t-ok" />verified payee</span><span><i className="dot" />other</span></span>
          </div>
          {stats.loading ? <Skeleton h={150} /> : s ? <>
            <Bars series={s.series} w={610} h={150} fluid />
            <div className="axis"><span>60 min ago</span><span>live minute ▸</span></div>
          </> : <Empty icon="∅" title="Stats unavailable" hint={stats.error?.message} />}
        </div>
        <TigerCard db={db} />
      </section>

      {(pay.loading || ledger.length > 0) && (
        <section className="card" aria-label="Ledger">
          <div className="card-head"><h2>Ledger</h2><span className="muted">{pay.loading ? '' : `${fmt(ledger.length)} rows loaded`}</span></div>
          {pay.loading ? <Skeleton variant="row" lines={6} /> : (
            <div className="table-wrap">
              <table className="table">
                <thead><tr><th>Time</th><th>Slot</th><th>From</th><th>To</th><th>Status</th>{st.data && <th>Capital One</th>}<th className="r">Amount</th><th>Signature</th></tr></thead>
                <tbody>
                  {ledger.map(p => (
                    <tr key={p.signature} className={sel?.signature === p.signature ? 'is-sel' : undefined}>
                      <td className="num" title={p.blockTime}>{clock(p.blockTime)}</td>
                      <td className="num">{p.slot}</td>
                      <td>{nameOf(p.payer)}</td>
                      <td>{nameOf(p.payee)}</td>
                      <td>{p.attackId ? <StatusDot status="signed" label="tampered" /> : <StatusDot status={p.payee.status} />}{p.source !== 'chain' && <span className="b-tag"> ~{p.source}</span>}</td>
                      {st.data && <td>{settled.has(p.signature) ? <SettleChip s={settled.get(p.signature)} /> : <span className="muted">—</span>}</td>}
                      <td className="r"><Amount raw={p.amountRaw} decimals={p.decimals} symbol={p.symbol} /></td>
                      <td><button type="button" className="hash copy" onClick={() => inspect(p)} aria-label={`Inspect transaction ${short(p.signature, 6)}`}>{short(p.signature, 6)}</button></td>
                    </tr>
                  ))}
                </tbody>
              </table>
            </div>
          )}
          {!pay.loading && ledger.length >= PAGE && more !== 'end' && (
            <button type="button" className="btn load-more" disabled={more === 'loading'} onClick={loadOlder}>
              {more === 'loading' ? 'Loading…' : more === 'error' ? 'Could not load. Try again' : 'Load older'}
            </button>
          )}
        </section>
      )}
    </Page>
  );
}
