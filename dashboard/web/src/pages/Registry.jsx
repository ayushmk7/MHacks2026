// Registry admin: issue and revoke attestations (badge public key -> verified name), each linked to its on-chain transaction.
import { useState } from 'react';
import './admin.css';
import { useApi, useData, useGaps, useStatus } from '../data.jsx';
import { Empty, ExplorerLink, GapCard, Page, Pubkey, Skeleton, StatusDot, ago, fmt, short, useNow } from '../ui.jsx';

// Mirrors `validate` in server/src/http.js, with the server's own messages.
// ponytail: the pubkey check is base58 shape only; the server does the real 32-byte decode.
const PUBKEY = /^[1-9A-HJ-NP-Za-km-z]{32,44}$/, NAME = /^[\x20-\x7E]{1,32}$/;
const day = iso => (iso ? new Date(iso).toLocaleDateString('en-GB', { day: 'numeric', month: 'short', year: 'numeric' }) : '—');

export default function Registry() {
  const now = useNow(5000);
  const { status, loading: booting } = useStatus(), gaps = useGaps('registry'), { post } = useApi();
  const badges = useData('badges'), atts = useData('attestations');
  const [target, setTarget] = useState(''), [pubkey, setPubkey] = useState(''), [name, setName] = useState('');
  const [busy, setBusy] = useState(''); // 'issue' | the subject being revoked
  const [error, setError] = useState(null), [done, setDone] = useState(null); // error: { at, code, message }; done: the POST response

  const reg = status?.registry, auth = status?.authority;
  const configured = badges.data?.badges.filter(b => b.pubkey) ?? [];
  // Checked against the list on every render: a badge that left badges.json must not stay selected behind the select's back.
  const to = target === 'other' || configured.some(b => b.pubkey === target) ? target : configured[0]?.pubkey ?? 'other';
  const other = to === 'other' && !badges.loading, subject = to === 'other' ? pubkey.trim() : to; // other = the public key field is on screen
  const rows = atts.data?.attestations ?? [];
  const who = a => a.label ?? short(a.subject);

  async function send(at, path, body) {
    setError(null); setDone(null); setBusy(at);
    try { setDone(await post(path, body)); if (at === 'issue') setName(''); }
    catch (err) { setError({ at, code: err.code, message: err.message }); }
    finally { setBusy(''); }
  }
  function issue(e) {
    e.preventDefault(); setDone(null);
    const body = { pubkey: subject, name: name.trim() };
    if (!PUBKEY.test(body.pubkey)) return setError({ at: 'issue', code: 'invalid_pubkey', message: 'pubkey must be a base58 32-byte address' });
    if (!NAME.test(body.name)) return setError({ at: 'issue', code: 'invalid_name', message: 'name must be 1 to 32 printable ASCII characters' });
    send('issue', '/api/attestations', body);
  }
  const revoke = a => confirm(`Revoke "${a.name}" for ${who(a)}?\n\nThis sends a transaction. Payments to this badge will show as REVOKED.`)
    && send(a.subject, '/api/attestations/revoke', { pubkey: a.subject });
  // A field error shows under its field; with the public key field off screen it falls through to the note above the table.
  const bad = field => (error?.at === 'issue' && error.code === `invalid_${field}` && (other || field === 'name') ? error.message : null);

  return (
    <Page title="Registry" subtitle="Bind a badge's public key to a verified name, on chain. Revoke it the same way.">
      {gaps.map(g => <GapCard key={g.code} gap={g} />)}

      <section className="card" aria-label="Registry on chain">
        <div className="card-head">
          <span className="label">Solana Attestation Service · {status?.cluster ?? 'devnet'}</span>
          {reg && <span className={`chip ${reg.ready ? 'chip--ok' : ''}`}>{reg.ready ? 'on chain' : 'created on first issue'}</span>}
        </div>
        {booting ? <Skeleton h="4.6rem" /> : !status ? <span className="muted">Registry details unavailable.</span> : (
          <dl className="kv">
            <div><dt>Credential</dt><dd>{reg?.credentialName ?? '—'}<br /><Pubkey value={reg?.credential} link /></dd></div>
            <div><dt>Schema</dt><dd>{reg?.schemaName ?? '—'}{reg?.schemaVersion != null && ` v${reg.schemaVersion}`}<br /><Pubkey value={reg?.schema} link /></dd></div>
            <div><dt>Authority · signs here</dt><dd className="num">{fmt(auth?.sol, 3)} SOL<br /><Pubkey value={auth?.pubkey} link /></dd></div>
            <div><dt>Program</dt><dd>SAS<br /><Pubkey value={reg?.program} link /></dd></div>
          </dl>
        )}
      </section>

      <section className="card" aria-label="Issue an attestation">
        <div className="card-head"><h2>Issue</h2><span className="muted">Issuing again to the same badge replaces its name.</span></div>
        <form className="grid issue" onSubmit={issue} noValidate>
          <label className="field">
            <span>Badge</span>
            <select className="select" value={to} onChange={e => { setTarget(e.target.value); setError(null); }} disabled={badges.loading}>
              {configured.map(b => <option key={b.id} value={b.pubkey}>{b.id} · {b.label} · {short(b.pubkey)}</option>)}
              <option value="other">Other public key…</option>
            </select>
          </label>
          {other && (
            <label className="field">
              <span>Public key</span>
              <input className="input" value={pubkey} onChange={e => setPubkey(e.target.value)} placeholder="base58 address" spellCheck="false" autoComplete="off"
                aria-invalid={bad('pubkey') ? 'true' : undefined} />
              {bad('pubkey') && <small className="field-error" role="alert">{bad('pubkey')}</small>}
            </label>
          )}
          <label className="field">
            <span>Verified name · {name.trim().length}/32</span>
            <input className="input" value={name} onChange={e => setName(e.target.value)} maxLength={32} placeholder="MHacks Merch" autoComplete="off"
              aria-invalid={bad('name') ? 'true' : undefined} />
            {bad('name') && <small className="field-error" role="alert">{bad('name')}</small>}
          </label>
          <div className="field">
            <span aria-hidden="true">&nbsp;</span>
            <button type="submit" className="btn btn--primary" disabled={Boolean(busy) || badges.loading}>{busy === 'issue' ? 'Sending…' : 'Issue attestation'}</button>
          </div>
        </form>
      </section>

      <section className="card" aria-label="Attestations">
        <div className="card-head"><h2>Attestations</h2><span className="muted">{!atts.data ? '' : `${rows.filter(a => a.status === 'verified').length} verified · ${rows.length} total`}</span></div>
        <div className="stack">
          {error && !bad('pubkey') && !bad('name') && (
            <div className="note note--bad" role="alert"><b>{error.at === 'issue' ? 'Issue failed.' : 'Revoke failed.'}</b> {error.message}</div>
          )}
          {done && (
            <div className="note note--ok row" role="status">
              <b>{done.attestation?.status === 'revoked' ? 'Revoked' : 'Issued'} “{done.attestation?.name}”</b>
              <span>{done.attestation?.status === 'revoked' ? 'for' : 'to'} {done.attestation ? who(done.attestation) : '—'}.</span>
              <ExplorerLink sig={done.signature}>View transaction</ExplorerLink>
            </div>
          )}
          {atts.loading ? <Skeleton variant="row" lines={3} />
            : !rows.length ? (atts.error ? <Empty icon="×" title={atts.error.code === 'offline' ? 'Backend offline' : 'Attestations unavailable'} hint={atts.error.message} />
              : <Empty title="No attestations yet" hint="Issue one above: it binds a badge's public key to a name the payer sees before paying." />)
            : (
              <div className="table-wrap">
                <table className="table">
                  {/* The action sits beside the status so Revoke stays on screen when a narrow window scrolls the table. */}
                  <thead><tr><th>Badge</th><th>Verified name</th><th>Status</th><th>Action</th><th>Issued</th><th>Revoked</th><th>Expires</th></tr></thead>
                  <tbody>
                    {rows.map(a => (
                      <tr key={a.subject}>
                        <td>{a.label ?? 'Unknown wallet'}<br /><Pubkey value={a.subject} link /></td>
                        <td><b>{a.name ?? '—'}</b></td>
                        <td><StatusDot status={a.status} /></td>
                        <td>{a.status === 'verified' && (
                          <button type="button" className="btn btn--danger btn--small" disabled={Boolean(busy)} onClick={() => revoke(a)}
                            aria-label={`Revoke ${a.name} for ${who(a)}`}>{busy === a.subject ? 'Revoking…' : 'Revoke'}</button>
                        )}</td>
                        <td><ExplorerLink sig={a.issuedSig} />{a.issuedAt && <><br /><span className="muted small">{ago(a.issuedAt, now)} ago</span></>}</td>
                        <td><ExplorerLink sig={a.revokedSig} />{a.revokedAt && <><br /><span className="muted small">{ago(a.revokedAt, now)} ago</span></>}</td>
                        <td className="num" title={a.expiresAt ?? undefined}>{day(a.expiresAt)}</td>
                      </tr>
                    ))}
                  </tbody>
                </table>
              </div>
            )}
        </div>
      </section>
    </Page>
  );
}
