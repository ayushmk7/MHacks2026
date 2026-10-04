// Routes: payments that reached the network through the relay mesh (Tier 0), drawn payer -> relays -> payee with each hop's
// verification and reward, plus the relay leaderboard. Live: SSE 'route' upserts a route by txSig (rewards pending -> paid).
import './admin.css';
import './routes.css';
import { useData, useGaps, useStatus } from '../data.jsx';
import { Amount, Empty, ExplorerLink, GapCard, Page, Pubkey, Skeleton, StatTile, ago, explorerTx, fmt, notBuilt, short, useNow } from '../ui.jsx';

const LIMIT = 50;
// rewardState -> [tone, words]. 'none' = an unverified relay: the network pays it nothing.
const REWARD = { paid: ['ok', 'paid'], pending: ['warn', 'pending'], failed: ['bad', 'failed'], none: ['mute', 'no reward'] };
const pkOf = v => (typeof v === 'string' ? v : v?.pubkey ?? null); // gateway: the contract leaves its shape open, take a key or a party
const hopsOf = r => [...(r.hops ?? [])].sort((a, b) => (a.position ?? 0) - (b.position ?? 0));
const nameOf = (x, fallback) => x?.name || x?.label || (x?.pubkey ? short(x.pubkey) : fallback);

// The leaderboard from the loaded routes, for a backend that has /api/routes but not /api/relays/leaderboard yet.
function derive(routes) {
  const m = new Map();
  for (const r of routes) for (const h of r.hops ?? []) {
    const e = m.get(h.pubkey) ?? { pubkey: h.pubkey, label: h.label, routes: 0, earned: 0 };
    e.routes++;
    if (h.rewardState === 'paid') e.earned = +(e.earned + (h.reward ?? 0)).toFixed(2);
    m.set(h.pubkey, e);
  }
  return [...m.values()].sort((a, b) => b.earned - a.earned || b.routes - a.routes);
}

function Node({ role, title, pubkey, tone, glyph, gateway, children, bad }) {
  return (
    <li className={`node${bad ? ' node--bad' : ''}`} style={tone ? { '--tone': `var(--${tone})` } : undefined}>
      <span className="row"><span className="label">{role}</span>{gateway && <span className="chip" title="The online relay that submitted the payment to Solana">gateway</span>}</span>
      <span className="node-name">{glyph && <span className="b-glyph" aria-hidden="true">{glyph}</span>}<span title={title}>{title}</span></span>
      <Pubkey value={pubkey} link />
      {children}
    </li>
  );
}

function Route({ r, now, symbol, decimals }) {
  const hops = hopsOf(r), gw = pkOf(r.gateway), href = r.links?.tx ?? (r.txSig ? explorerTx(r.txSig) : null);
  const unverified = hops.filter(h => !h.verified).length;
  return (
    <article className="route rise" aria-label={`${fmt(r.amount)} ${symbol} from ${nameOf(r.payer, 'payer')} to ${nameOf(r.payee, 'payee')} via ${hops.length} relays`}>
      <div className="route-head">
        <span className="route-amt">{r.amountRaw != null ? <Amount raw={r.amountRaw} decimals={decimals} symbol={symbol} /> : <span className="num amount">{fmt(r.amount)}<small> {symbol}</small></span>}</span>
        <span className="route-flow">{nameOf(r.payer, 'payer')} <span aria-hidden="true">→</span> {nameOf(r.payee, 'payee')}</span>
        <span className="chip">{hops.length} {hops.length === 1 ? 'relay' : 'relays'}</span>
        {r.e2eProof ? <span className="chip chip--ok" title="The payer's confirmation came back end to end through the mesh">end-to-end proof</span>
          : r.e2eProof === false ? <span className="chip" title="No end-to-end confirmation reached the payer; the merchant's PAID is the authoritative one">no end-to-end proof</span> : null}
        {unverified > 0 && <span className="chip chip--bad">{unverified} unverified {unverified === 1 ? 'hop' : 'hops'}</span>}
        <span className="grow" />
        <span className="muted num" title={r.time}>{ago(r.time, now)} ago</span>
        {href ? <a className="ext-link" href={href} target="_blank" rel="noreferrer" title={r.txSig}>{short(r.txSig, 6)}<span aria-hidden="true"> ↗</span></a> : <span className="hash">—</span>}
      </div>
      <ol className="path" aria-label="Route, payer first">
        <Node role="Payer" title={nameOf(r.payer, 'Unknown wallet')} pubkey={r.payer?.pubkey} />
        {hops.map(h => {
          const [tone, words] = REWARD[h.rewardState] ?? ['mute', h.rewardState ?? '—'];
          return (
            <Node key={`${h.position}-${h.pubkey}`} role={`Relay ${h.position ?? ''}`} title={h.label ?? (h.verified ? 'Verified relay' : 'Unknown relay')}
              pubkey={h.pubkey} tone={h.verified ? 'ok' : 'bad'} glyph={h.verified ? '✓' : '✗'} gateway={gw && gw === h.pubkey} bad={!h.verified}>
              <span className={`small ${h.verified ? 't-ok' : 't-bad'}`}>{h.verified ? 'verified relay' : 'unverified: no attestation'}</span>
              <span className="node-reward">
                <span className="num">{h.reward ? `+${fmt(h.reward, 2)} ${symbol}` : '—'}</span>
                <span className={`status t-${tone}`}><i className="dot" aria-hidden="true" /><span>{words}</span></span>
              </span>
              {h.rewardSig && <ExplorerLink sig={h.rewardSig}>reward tx</ExplorerLink>}
            </Node>
          );
        })}
        <Node role="Payee" title={nameOf(r.payee, 'Unknown wallet')} pubkey={r.payee?.pubkey} />
      </ol>
    </article>
  );
}

function Board({ lb, routes, symbol }) {
  const derived = notBuilt(lb.error) && !lb.data;
  const rows = lb.data?.relays ?? (derived ? derive(routes) : []);
  const verified = new Map(routes.flatMap(r => r.hops ?? []).map(h => [h.pubkey, h.verified]));
  return (
    <section className="card board" aria-label="Relay leaderboard">
      <div className="card-head"><h2>Relay leaderboard</h2>{derived ? <span className="chip" title="This backend has no /api/relays/leaderboard yet">from loaded routes</span> : <span className="muted">0.01 {symbol} per verified hop</span>}</div>
      {lb.loading && !derived ? <Skeleton variant="row" lines={4} />
        : !rows.length ? (lb.error && !derived ? <Empty icon="×" title={lb.error.code === 'offline' ? 'Backend offline' : 'Leaderboard unavailable'} hint={lb.error.message} />
          : <Empty title="No relays yet" hint="A relay shows up here after it carries its first payment." />)
        : (
          <div className="table-wrap">
            <table className="table">
              <thead><tr><th>#</th><th>Relay</th><th className="r">Routes</th><th className="r">Earned</th></tr></thead>
              <tbody>
                {rows.map((x, i) => {
                  const v = verified.get(x.pubkey);
                  return (
                    <tr key={x.pubkey}>
                      <td className="rank">{i + 1}</td>
                      <td>
                        <span className="row">
                          {v != null && <span className={`b-glyph ${v ? 't-ok' : 't-bad'}`} title={v ? 'verified relay' : 'unverified relay'}>{v ? '✓' : '✗'}</span>}
                          <b>{x.label ?? 'Unknown relay'}</b>
                        </span>
                        <Pubkey value={x.pubkey} link />
                      </td>
                      <td className="r num">{fmt(x.routes)}</td>
                      <td className="r num"><b>{fmt(x.earned, 2)}</b> <span className="muted small">{symbol}</span></td>
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

export default function Routes() {
  const now = useNow();
  const { status } = useStatus(), gaps = useGaps('routes');
  const rt = useData('routes', { limit: LIMIT }), lb = useData('relays/leaderboard');
  const symbol = status?.token?.symbol ?? 'HACK', decimals = status?.token?.decimals ?? 2, routes = rt.data?.routes ?? [];
  const hops = routes.flatMap(r => r.hops ?? []);
  const paid = hops.filter(h => h.rewardState === 'paid').reduce((a, h) => a + (h.reward ?? 0), 0);
  const missing = notBuilt(rt.error) && !rt.data;

  return (
    <Page title="Routes" subtitle="Payments that hopped across the relay mesh to reach Solana. Relays can delay a payment, never change it; verified relays earn HACK.">
      {gaps.map(g => <GapCard key={g.code} gap={g} />)}

      {!missing && (
        <section className="tiles" aria-label="Loaded routes">
          {rt.loading ? [0, 1, 2, 3].map(i => <Skeleton key={i} variant="card" />) : <>
            <StatTile label="Routed payments" value={fmt(routes.length)} sub={routes.length >= LIMIT ? `latest ${LIMIT}` : 'through the mesh'} />
            <StatTile label="Verified hops" value={hops.length ? `${Math.round((hops.filter(h => h.verified).length / hops.length) * 100)}%` : '—'}
              tone={!hops.length ? undefined : hops.every(h => h.verified) ? 'ok' : 'warn'} sub={`${fmt(hops.filter(h => !h.verified).length)} unverified, no reward`} />
            <StatTile label={`${symbol} to relays`} value={fmt(paid, 2)} sub={`${fmt(hops.filter(h => h.rewardState === 'pending').length)} rewards pending`} />
            <StatTile label="End-to-end proof" value={routes.length ? `${Math.round((routes.filter(r => r.e2eProof).length / routes.length) * 100)}%` : '—'}
              sub="payer saw the confirmation" />
          </>}
        </section>
      )}

      <section className="split">
        <section className="card" aria-label="Routed payments">
          <div className="card-head">
            <h2>Routed payments</h2>
            <span className="legend" aria-label="Hop states">
              <span className="t-ok"><span className="b-glyph">✓</span>verified relay</span>
              <span className="t-bad"><span className="b-glyph">✗</span>unverified</span>
            </span>
          </div>
          {rt.loading ? <Skeleton variant="row" lines={6} />
            : missing ? <Empty icon="…" title="Routes not built yet" hint="This backend has no `/api/routes` yet. Payments that hop through relay badges appear here once it ships." />
            : !routes.length ? (rt.error ? <Empty icon="×" title={rt.error.code === 'offline' ? 'Backend offline' : 'Routes unavailable'} hint={rt.error.message} />
              : <Empty title="No routed payments yet" hint="Take the judge's badge offline and pay through the relays: the route lands here when the gateway reports it." />)
            : <div>{routes.map(r => <Route key={r.txSig} r={r} now={now} symbol={symbol} decimals={decimals} />)}</div>}
        </section>
        <Board lb={lb} routes={routes} symbol={symbol} />
      </section>
    </Page>
  );
}
