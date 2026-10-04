// Badges: the four badge slots. Identity, where the signing key lives, balances (HACK and bank), attestation status, bank enrollment,
// and top-ups from Capital One (a Nessie withdrawal, then HACK sent to the badge). Live: SSE 'topup' moves a top-up pending -> done.
import { useId, useState } from 'react';
import './admin.css';
import { useApi, useData, useGaps, useStatus } from '../data.jsx';
import { Empty, GapCard, Page, Pubkey, Skeleton, StatusDot, ago, fmt, notBuilt, short, usd, useNow } from '../ui.jsx';

const WHOLE = /^\d+$/;
const clock = iso => new Date(iso).toLocaleTimeString('en-GB');
// done reads as the green 'approved' dot, labelled 'done'.
const TopupStatus = ({ t }) => <StatusDot status={t.status === 'done' ? 'approved' : t.status} label={t.status} />;
const TOPUP_REASON = { insufficient_funds: 'Not enough dollars in the Capital One account', nessie_error: 'Bank call failed', not_enrolled: 'No Capital One account' };
const topupReason = r => (r ? TOPUP_REASON[r] ?? r.replace(/_/g, ' ') : null);

/** Top up from Capital One: whole HACK, 1..1000. The row it creates is followed live through the topups list. */
function TopUp({ b, symbol, topups }) {
  const { post } = useApi(), id = useId();
  const [amount, setAmount] = useState('100'), [busy, setBusy] = useState(false);
  const [sent, setSent] = useState(null), [error, setError] = useState(null); // sent: the POST response
  const n = Number(amount.trim()), valid = WHOLE.test(amount.trim()) && n >= 1 && n <= 1000;
  const live = sent && (topups.find(t => t.id === sent.id) ?? sent);
  async function go(e) {
    e.preventDefault(); setError(null);
    if (!valid) return setError({ field: true, message: `Whole ${symbol} only, from 1 to 1000.` });
    setBusy(true); setSent(null);
    try { setSent(await post('/api/topups', { pubkey: b.pubkey, amount: n })); }
    catch (err) { setError({ message: notBuilt(err) ? 'This backend has no /api/topups yet.' : err.message }); }
    finally { setBusy(false); }
  }
  return (
    <form className="stack" onSubmit={go} noValidate style={{ gap: 'var(--sp-1)' }}>
      <div className="field">
        <label htmlFor={id}>Top up from Capital One · whole {symbol}</label>
        <div className="row" style={{ flexWrap: 'nowrap' }}>
          <input id={id} className="input" value={amount} onChange={e => { setAmount(e.target.value); setError(null); }} inputMode="numeric" autoComplete="off"
            aria-invalid={error?.field ? 'true' : undefined} />
          <button type="submit" className="btn" disabled={busy} style={{ minHeight: '2.75rem' }}>{busy ? 'Sending…' : 'Top up'}</button>
        </div>
        {error?.field ? <small className="field-error" role="alert">{error.message}</small>
          : <small className="muted small">Withdraws {valid ? usd(n * 100) : 'the dollars'} from the Nessie account and sends {valid ? fmt(n) : 'the same'} {symbol} to this badge.</small>}
      </div>
      {error && !error.field && <div className="note note--bad" role="alert"><b>Top-up failed.</b> {error.message}</div>}
      {live && (
        <div className={`note row ${live.status === 'done' ? 'note--ok' : live.status === 'failed' ? 'note--bad' : 'note--warn'}`} role="status">
          <TopupStatus t={live} />
          <span className="grow">{fmt(live.amount)} {symbol}{live.status === 'pending' ? ', waiting for the bank' : live.reason ? ` · ${topupReason(live.reason)}` : ''}</span>
          {live.links?.tx && <a className="ext-link" href={live.links.tx} target="_blank" rel="noreferrer" title={live.hackSig ?? live.links.tx}>tx<span aria-hidden="true"> ↗</span></a>}
        </div>
      )}
    </form>
  );
}

function Topups({ tp, badges, symbol }) {
  const now = useNow(5000), rows = tp.data?.topups ?? [];
  const labelOf = pk => badges.find(b => b.pubkey === pk)?.label ?? short(pk);
  if (notBuilt(tp.error) && !tp.data) return (
    <section className="card" aria-label="Top-ups">
      <div className="card-head"><h2>Top-ups</h2></div>
      <p className="muted">This backend has no <code>/api/topups</code> yet. Top-ups from Capital One appear here once it ships.</p>
    </section>
  );
  return (
    <section className="card" aria-label="Top-ups">
      <div className="card-head"><h2>Top-ups</h2><span className="muted">Capital One withdrawal → {symbol} from the treasury</span></div>
      {tp.loading ? <Skeleton variant="row" lines={3} />
        : !rows.length ? (tp.error ? <Empty icon="×" title={tp.error.code === 'offline' ? 'Backend offline' : 'Top-ups unavailable'} hint={tp.error.message} />
          : <Empty title="No top-ups yet" hint="Top up an enrolled badge above: dollars leave its Capital One account and the same amount of HACK lands on the badge." />)
        : (
          <div className="table-wrap">
            <table className="table">
              <thead><tr><th>Time</th><th>Badge</th><th>Status</th><th className="r">Amount</th><th>Nessie withdrawal</th><th>Transfer</th></tr></thead>
              <tbody>
                {rows.map(t => (
                  <tr key={t.id}>
                    <td className="num" title={t.time}>{clock(t.time)}<br /><span className="muted small">{ago(t.time, now)} ago</span></td>
                    <td>{labelOf(t.pubkey)}</td>
                    <td><TopupStatus t={t} />{t.reason && <><br /><span className="small t-bad" title={t.reason}>{topupReason(t.reason)}</span></>}</td>
                    <td className="r num">{usd(t.amount * 100)} → {fmt(t.amount)} {symbol}</td>
                    <td className="hash">{t.nessieWithdrawalId ? <span title={t.nessieWithdrawalId}>{short(t.nessieWithdrawalId, 6)}</span> : '—'}</td>
                    <td>{t.links?.tx ? <a className="ext-link" href={t.links.tx} target="_blank" rel="noreferrer" title={t.hackSig ?? t.links.tx}>{short(t.hackSig, 6)}<span aria-hidden="true"> ↗</span></a> : <span className="hash">—</span>}</td>
                  </tr>
                ))}
              </tbody>
            </table>
          </div>
        )}
    </section>
  );
}

// BADGE-GAP(key-location): each badge reports where its key lives (Settings > Identity); the value is copied into
// server/config/badges.json. Anything else than se050 / software reads UNKNOWN: the panel never claims a secure element.
const KEY = { se050: ['SECURE ELEMENT', 'chip--ok'], software: ['SOFTWARE', 'chip--warn'] };

function Badge({ b, symbol, topups }) {
  const [where, tone] = KEY[b.keyLocation] ?? ['UNKNOWN', ''];
  const att = b.attestation ?? {}, bal = fmt(b.hack);
  const head = (
    <div className="row">
      <span className="badge-no" aria-hidden="true">{b.id}</span>
      <h2 className="grow">{b.label ?? `Badge ${b.id}`}</h2>
    </div>
  );
  // BADGE-GAP(badge-pubkeys): no public key for this slot yet. Show the hole and the fix, never fake balances.
  if (!b.pubkey) return (
    <article className="card stack badge--waiting" aria-label={`Badge ${b.id}, waiting for badge`}>
      {head}
      <div className="badge-wait">Waiting for badge</div>
      <p className="muted">Paste this badge's public key into <code>server/config/badges.json</code> (Settings &gt; Identity on the badge).</p>
      <div><span className="chip">BADGE-GAP(badge-pubkeys)</span></div>
    </article>
  );
  return (
    <article className="card stack" aria-label={`Badge ${b.id}, ${b.label}`}>
      {head}
      <div className="row">
        <StatusDot status={att.status ?? 'unverified'} />
        <span>{att.name ?? 'no verified name'}</span>
        {att.kind && <span className="chip">{att.kind}</span>}
      </div>
      {/* --n: characters in the balance; admin.css shrinks a long one so it stays inside the card. */}
      <div className={`stat-value num badge-bal${b.hack == null ? ' t-mute' : ''}`} style={{ '--n': bal.length }}>{bal}<small className="label"> {symbol}</small></div>
      <dl className="kv kv--tight">
        <div><dt>Key location</dt><dd><span className={`chip ${tone}`}>{where}</span></dd></div>
        {b.standIn && <div><dt>Hardware</dt><dd><span className="chip" title="Software keypair made by npm run devnet:setup, standing in for the real badge">stand-in keypair</span></dd></div>}
        <div><dt>Public key</dt><dd><Pubkey value={b.pubkey} link /></dd></div>
        <div><dt>SOL for fees</dt><dd className="num">{fmt(b.sol, 4)}</dd></div>
        <div><dt>Payments in</dt><dd className="num">{fmt(b.received?.count)}</dd></div>
        <div><dt>Received</dt><dd className="num">{fmt(b.received?.amount)} {symbol}</dd></div>
        {/* nessie absent = a backend that does not report the bank yet: no row. null = not enrolled. */}
        {b.nessie !== undefined && <div><dt>Bank · Nessie</dt><dd>{b.nessie ? <span className="chip chip--ok">enrolled</span>
          : <span className="chip" title="Enroll it on the Issuer page">not enrolled</span>}</dd></div>}
        {b.nessie && <div><dt>Bank balance</dt><dd className="num"><b>{usd(b.nessie.usdCents)}</b></dd></div>}
        {b.nessie && <div><dt>Nessie account</dt><dd className="hash" title={b.nessie.accountId}>{short(b.nessie.accountId, 6)}</dd></div>}
      </dl>
      {b.nessie ? <TopUp b={b} symbol={symbol} topups={topups} />
        : b.nessie === null && <p className="muted small">Enroll this badge at the bank on the Issuer page to top up {symbol} from Capital One.</p>}
    </article>
  );
}

export default function Badges() {
  const { status } = useStatus(), gaps = useGaps('badges'), { data, loading, error } = useData('badges');
  const symbol = status?.token?.symbol ?? 'HACK', list = data?.badges ?? [];
  const tp = useData('topups', { limit: 20 }), topups = tp.data?.topups ?? [];
  return (
    <Page title="Badges" subtitle="Four badges: where each key lives, what it holds on chain and at the bank, whether its name is verified. Top up HACK from Capital One.">
      {gaps.map(g => <GapCard key={g.code} gap={g} />)}
      {loading || list.length ? (
        <section className="grid badges" aria-label="Badges">
          {loading ? [0, 1, 2, 3].map(i => <Skeleton key={i} variant="card" h="22rem" />) : list.map(b => <Badge key={b.id} b={b} symbol={symbol} topups={topups} />)}
        </section>
      ) : error ? <Empty icon="×" title={error.code === 'offline' ? 'Backend offline' : 'Badges unavailable'} hint={error.message} />
        : <Empty title="No badge slots" hint="`server/config/badges.json` has no entries." />}
      {(list.some(b => b.nessie) || topups.length > 0) && <Topups tp={tp} badges={list} symbol={symbol} />}
    </Page>
  );
}
