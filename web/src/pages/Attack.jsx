// Attack console (PRD F12): a "malicious merchant" checkout that claims one amount while the transaction it hands the
// badge moves another. It only targets the team's own configured badges, on devnet, with the demo token. The point of
// the demo is the defence: the badge decodes the real amount on its own screen and the holder refuses.
import { useState } from 'react';
import './admin.css';
import { useApi, useData, useGaps, useStatus } from '../data.jsx';
import { Empty, ExplorerLink, GapCard, Page, Pubkey, Skeleton, StatusDot, ago, fmt, short, useCopy, useNow } from '../ui.jsx';

// outcome -> [note tone, what it means]. rejected = the defence held, signed = the tampered transfer landed.
const OUTCOME = {
  pending: ['warn', 'Waiting for the badge.'],
  rejected: ['ok', 'The holder saw the real amount on the badge and refused. The defence held.'],
  signed: ['bad', 'The tampered transfer was signed and landed on chain.'],
  expired: ['', 'Nobody signed before the blockhash expired.'],
};

function Versus({ a, now, busy, onReject }) {
  const [copied, copy] = useCopy();
  const claimed = fmt(a.displayAmount, a.decimals), actual = fmt(a.decoded?.amount ?? a.actualAmount, a.decimals);
  const sm = Math.max(claimed.length, actual.length) > 4 ? ' versus-amt--sm' : '';
  const [tone, meaning] = OUTCOME[a.outcome] ?? OUTCOME.expired, left = Math.round((Date.parse(a.expiresAt) - now) / 1000);
  return (
    <div className="stack">
      <div className="versus">
        <div className="versus-side">
          <div className="label">The checkout claims</div>
          <div className={`versus-amt${sm}`}>{claimed}<small>{a.symbol}</small></div>
          <div className="muted">shown on the merchant's screen</div>
        </div>
        <div className="versus-ne" aria-hidden="true">≠</div>
        <div className="versus-side versus-side--bad t-bad" role="alert">
          <div className="label t-bad">The transaction moves</div>
          <div className={`versus-amt${sm}`}>{actual}<small>{a.symbol}</small></div>
          <div><b>{fmt(a.actualAmount / a.displayAmount, 1)}× the claim</b> · decoded from the bytes sent to the badge</div>
        </div>
      </div>

      <div className={`note${tone ? ` note--${tone}` : ''} row`}>
        <StatusDot status={a.outcome} />
        <span className="grow">{meaning}{a.outcome === 'pending' && (left > 0 ? ` Blockhash expires in about ${left}s.` : ' The blockhash has probably expired.')}</span>
        {a.outcome === 'signed' && <ExplorerLink sig={a.signature}>View transaction</ExplorerLink>}
        {/* BADGE-GAP(attack-outcome): the badge does not report a refusal yet, so a human marks it. 'signed' only ever comes from chain. */}
        {a.outcome === 'pending' && <button type="button" className="btn" disabled={Boolean(busy)} onClick={() => onReject(a)}>{busy === a.id ? 'Marking…' : 'Mark rejected'}</button>}
      </div>
      {a.warnings?.map(w => <div key={w} className="note note--warn">{w}</div>)}

      <dl className="kv">
        <div><dt>Victim · must sign</dt><dd>{a.victim?.label ?? 'Unknown badge'}<br /><Pubkey value={a.victim?.pubkey} link /></dd></div>
        <div><dt>Destination · attacker</dt><dd>token account <Pubkey value={a.decoded?.destination} link /><br />owner <Pubkey value={a.attacker} link /></dd></div>
        <div><dt>Instruction</dt><dd className="num">{a.decoded?.program ?? '—'}<br />{a.decoded?.instruction ?? '—'}</dd></div>
        <div><dt>Transaction</dt><dd className="num">{fmt(a.txBytes)} bytes<br />version {a.txVersion ?? '—'}</dd></div>
      </dl>
      {/* BADGE-GAP(attack-payload): which message version the badge decoder parses is unknown until firmware exists. */}
      <div className="row muted small">
        <span className="chip">BADGE-GAP(attack-payload)</span>
        <span>Built as a <b>{a.txVersion ?? '—'}</b> message. Set <code>ATTACK_TX_VERSION</code> in <code>.env</code> to <code>legacy</code> or <code>0</code>, whichever the badge decoder parses.</span>
      </div>
      <details>
        <summary className="label">Unsigned transaction · base64</summary>
        <div className="sig">
          <span className="hash small">{a.txBase64}</span>
          <button type="button" className="btn btn--small" onClick={() => copy(a.txBase64)}>{copied ? 'Copied' : 'Copy'}</button>
        </div>
      </details>
    </div>
  );
}

export default function Attack() {
  const now = useNow();
  const { status } = useStatus(), gaps = useGaps('attack'), { post } = useApi();
  const badges = useData('badges'), attacks = useData('attacks');
  const [victim, setVictim] = useState(''), [claimed, setClaimed] = useState('5'), [actual, setActual] = useState('500');
  const [selId, setSelId] = useState(null); // the attempt on show: just built or clicked in the list; null = the newest
  const [busy, setBusy] = useState(''); // 'build' | the id being marked
  const [error, setError] = useState(null); // { at: 'build' | 'reject', message }
  const [copied, copy] = useCopy();

  const symbol = status?.token?.symbol ?? 'HACK', step = 1 / 10 ** (status?.token?.decimals ?? 2), listener = status?.badgeListener;
  const configured = badges.data?.badges.filter(b => b.pubkey) ?? [];
  // ponytail: the default victim is found by its label ("Judge …", as in badges.json); falls back to the first badge.
  const target = victim || (configured.find(b => /judge/i.test(b.label)) ?? configured[0])?.pubkey || '';
  const attempts = attacks.data?.attempts ?? [], shown = attempts.find(x => x.id === selId) ?? attempts[0];
  const deliveryGap = gaps.find(g => g.code === 'attack_delivery_unconfigured'), outcomeGap = gaps.find(g => g.code === 'attack_outcome_manual');
  const blocked = gaps.some(g => g.severity === 'blocker');
  const pollUrl = listener?.url && `${listener.url}?badge=${shown?.victim?.pubkey ?? target}`;

  // Both actions wait for the list to reload before they finish, so the panel never flashes the previous state.
  async function send(at, path, body) {
    setError(null); setBusy(body.id ?? at);
    try { const out = await post(path, body); await attacks.refetch(); setSelId(out.id); }
    catch (err) { setError({ at, message: err.message }); }
    finally { setBusy(''); }
  }
  const build = e => { e.preventDefault(); send('build', '/api/attacks', { victim: target, displayAmount: Number(claimed), actualAmount: Number(actual) }); };
  const reject = a => send('reject', '/api/attacks/outcome', { id: a.id, outcome: 'rejected' });

  return (
    <Page title="Attack console" subtitle="A dishonest checkout shows one amount and asks the badge to sign another. The badge screen is the defence.">
      {gaps.filter(g => g !== deliveryGap && g !== outcomeGap).map(g => <GapCard key={g.code} gap={g} />)}

      <div className="split split--form">
        <section className="card" aria-label="Fake checkout">
          <div className="card-head"><h2>Fake checkout</h2><span className="chip chip--bad">malicious merchant</span></div>
          {badges.loading ? <Skeleton variant="row" lines={4} /> : (
            // ponytail: native number validation (min/max/step); the server re-checks and its message shows below.
            <form className="stack" onSubmit={build}>
              <label className="field">
                <span>Victim badge</span>
                <select className="select" value={target} onChange={e => setVictim(e.target.value)} disabled={!configured.length}>
                  {!configured.length && <option value="">no badge configured</option>}
                  {configured.map(b => <option key={b.id} value={b.pubkey}>{b.id} · {b.label} · {short(b.pubkey)}</option>)}
                </select>
              </label>
              <label className="field">
                <span>Checkout shows · {symbol}</span>
                <input className="input" type="number" inputMode="decimal" min={step} max="1000000" step={step} required value={claimed} onChange={e => setClaimed(e.target.value)} />
              </label>
              <label className="field">
                <span>Transaction really moves · {symbol}</span>
                <input className="input" type="number" inputMode="decimal" min={step} max="1000000" step={step} required value={actual} onChange={e => setActual(e.target.value)} />
              </label>
              <button type="submit" className="btn btn--danger" disabled={Boolean(busy) || blocked || !target}>{busy === 'build' ? 'Building…' : `Charge ${claimed || '?'} ${symbol}`}</button>
              {error?.at === 'build' && <div className="note note--bad" role="alert"><b>Could not build the charge.</b> {error.message}</div>}
              <p className="muted small">Builds an unsigned transfer for this badge only. Nothing reaches the chain unless the badge holder signs it.</p>
            </form>
          )}
        </section>

        <section className="card" aria-label="Claimed versus actual">
          <div className="card-head">
            <h2>What you see ≠ what you sign</h2>
            {shown && <span className="muted">attempt <span className="mono">{short(shown.id, 4)}</span> · {ago(shown.createdAt, now)} ago</span>}
          </div>
          {attacks.loading ? <Skeleton h="15rem" />
            : shown ? <Versus a={shown} now={now} busy={busy} onReject={reject} />
            : attacks.error ? <Empty icon="×" title={attacks.error.code === 'offline' ? 'Backend offline' : 'Attempts unavailable'} hint={attacks.error.message} />
            : <Empty title="No attempts yet" hint="Charge a badge from the fake checkout: the claimed amount and the real one appear here side by side." />}
          {error?.at === 'reject' && <div className="note note--bad" role="alert"><b>Could not mark it rejected.</b> {error.message}</div>}
        </section>
      </div>

      <div className="split split--form">
        <section className="card stack delivery" aria-label="Delivery to the badge">
          <div className="card-head">
            <h2>Delivery</h2>
            <StatusDot status={listener?.open ? 'live' : 'idle'} label={!listener ? 'unknown' : listener.open ? 'listener open' : 'listener closed'} />
          </div>
          {/* BADGE-GAP(attack-delivery): how the transaction reaches the badge is not settled. Stub transport: the badge
              polls the laptop over hotspot Wi-Fi (GET /badge/pending). BLE is the alternative. */}
          {listener?.open ? <>
            <p>The badge polls this address over the hotspot and receives the pending charge for its key.</p>
            <div className="sig">
              <span className="hash">{pollUrl}</span>
              <button type="button" className="btn btn--small" onClick={() => copy(pollUrl)}>{copied ? 'Copied' : 'Copy'}</button>
            </div>
          </> : !listener ? <p className="muted">The listener state comes from the backend, which is not answering.</p>
            : deliveryGap ? <GapCard gap={deliveryGap} /> : <>
            <p className="muted">Nothing is handed to a badge. Set <code>BADGE_LISTEN_HOST</code> in <code>.env</code> to the laptop's hotspot IP to open the badge-only listener.</p>
            <div><span className="chip">BADGE-GAP(attack-delivery)</span></div>
          </>}
        </section>

        <section className="card" aria-label="Attempts">
          <div className="card-head"><h2>Attempts</h2><span className="muted">{attacks.data ? `${attempts.length} built` : ''}</span></div>
          {/* BADGE-GAP(attack-outcome): 'signed' is detected from chain by itself; 'rejected' is a manual mark until the badge reports it. */}
          {outcomeGap && <GapCard gap={outcomeGap} />}
          {attacks.loading ? <Skeleton variant="row" lines={3} /> : attempts.length > 0 && (
            <div className="table-wrap">
              <table className="table">
                <thead><tr><th>Attempt</th><th>Victim</th><th className="r">Claimed</th><th className="r">Actual</th><th>Outcome</th><th>Signature</th><th><span className="muted">Action</span></th></tr></thead>
                <tbody>
                  {attempts.map(a => (
                    <tr key={a.id} className={a.id === shown?.id ? 'is-sel' : undefined}>
                      <td><button type="button" className="hash copy" onClick={() => setSelId(a.id)} aria-label={`Show attempt ${short(a.id, 4)}`}>{short(a.id, 4)}</button> <span className="muted small">{ago(a.createdAt, now)} ago</span></td>
                      <td>{a.victim?.label ?? short(a.victim?.pubkey)}</td>
                      <td className="r num">{fmt(a.displayAmount, a.decimals)}</td>
                      <td className="r num t-bad"><b>{fmt(a.actualAmount, a.decimals)}</b></td>
                      <td><StatusDot status={a.outcome} /></td>
                      <td><ExplorerLink sig={a.signature} /></td>
                      <td>{a.outcome === 'pending' && (
                        <button type="button" className="btn btn--small" disabled={Boolean(busy)} onClick={() => reject(a)}
                          aria-label={`Mark attempt ${short(a.id, 4)} rejected`}>{busy === a.id ? 'Marking…' : 'Mark rejected'}</button>
                      )}</td>
                    </tr>
                  ))}
                </tbody>
              </table>
            </div>
          )}
          {attacks.data && !attempts.length && <p className="muted">Nothing built yet.</p>}
        </section>
      </div>
    </Page>
  );
}
