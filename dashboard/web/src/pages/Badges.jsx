// Badges: the four badge slots. Identity, where the signing key lives, balances, attestation status.
import './admin.css';
import { useData, useGaps, useStatus } from '../data.jsx';
import { Empty, GapCard, Page, Pubkey, Skeleton, StatusDot, fmt } from '../ui.jsx';

// BADGE-GAP(key-location): each badge reports where its key lives (Settings > Identity); the value is copied into
// server/config/badges.json. Anything else than se050 / software reads UNKNOWN: the panel never claims a secure element.
const KEY = { se050: ['SECURE ELEMENT', 'chip--ok'], software: ['SOFTWARE', 'chip--warn'] };

function Badge({ b, symbol }) {
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
      </dl>
    </article>
  );
}

export default function Badges() {
  const { status } = useStatus(), gaps = useGaps('badges'), { data, loading, error } = useData('badges');
  const symbol = status?.token?.symbol ?? 'HACK', list = data?.badges ?? [];
  return (
    <Page title="Badges" subtitle="Four badges: where each key lives, what it holds, and whether its name is verified.">
      {gaps.map(g => <GapCard key={g.code} gap={g} />)}
      {loading || list.length ? (
        <section className="grid badges" aria-label="Badges">
          {loading ? [0, 1, 2, 3].map(i => <Skeleton key={i} variant="card" h="22rem" />) : list.map(b => <Badge key={b.id} b={b} symbol={symbol} />)}
        </section>
      ) : error ? <Empty icon="×" title={error.code === 'offline' ? 'Backend offline' : 'Badges unavailable'} hint={error.message} />
        : <Empty title="No badge slots" hint="`server/config/badges.json` has no entries." />}
    </Page>
  );
}
