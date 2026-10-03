// Feed: stat tiles, the block-chain strip (one payment = one block), block detail, minute bars, Tiger Data card, ledger.
import { useLayoutEffect, useRef, useState } from 'react';
import { useApi, useData, useGaps, useMode, useStatus } from '../data.jsx';
import { Amount, Bars, Empty, ExplorerLink, GapCard, Page, Pubkey, Skeleton, StatTile, StatusDot, ago, fmt, short, useCopy, useNow } from '../ui.jsx';

const GLYPH = { verified: '✓', unverified: '?', revoked: '⊘' };
const PAGE = 40, RAIL = 30;
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
  const blocks = rows.slice(0, RAIL);
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

function Detail({ p, now, onClose }) {
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
      </dl>
    </div>
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
  const pay = useData('payments'), stats = useData('stats'), db = useData('stats/db');
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
    <Page title="Payments" subtitle={`Every ${symbol} transfer is a block. The newest joins the chain at the head.`}>
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
        {(pay.loading || rows.length > 0) && <Detail p={sel} now={now} onClose={() => setSel(null)} />}
      </section>

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
                <thead><tr><th>Time</th><th>Slot</th><th>From</th><th>To</th><th>Status</th><th className="r">Amount</th><th>Signature</th></tr></thead>
                <tbody>
                  {ledger.map(p => (
                    <tr key={p.signature} className={sel?.signature === p.signature ? 'is-sel' : undefined}>
                      <td className="num" title={p.blockTime}>{clock(p.blockTime)}</td>
                      <td className="num">{p.slot}</td>
                      <td>{nameOf(p.payer)}</td>
                      <td>{nameOf(p.payee)}</td>
                      <td>{p.attackId ? <StatusDot status="signed" label="tampered" /> : <StatusDot status={p.payee.status} />}{p.source !== 'chain' && <span className="b-tag"> ~{p.source}</span>}</td>
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
