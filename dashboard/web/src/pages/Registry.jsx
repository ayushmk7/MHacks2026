// Issuer (Capital One) admin: issue and revoke payee attestations (badge public key -> verified name + kind + settle mode), each linked
// to its on-chain transaction, and enroll badges at the bank (Nessie). Only Capital One account holders can be verified.
import { useState } from 'react';
import './admin.css';
import { useApi, useData, useGaps, useStatus } from '../data.jsx';
import { Empty, ExplorerLink, GapCard, Page, Pubkey, Skeleton, StatusDot, ago, fmt, notBuilt, short, usd, useNow } from '../ui.jsx';

// Mirrors `validate` in server/src/http.js, with the server's own messages.
// ponytail: the pubkey check is base58 shape only; the server does the real 32-byte decode.
const PUBKEY = /^[1-9A-HJ-NP-Za-km-z]{32,44}$/, NAME = /^[\x20-\x7E]{1,32}$/;
const KINDS = [['merchant', 'Merchant'], ['person', 'Person']];
// Capital One's settlement role: where a verified merchant or person is paid.
const SETTLE = [['hack', 'Receive HACK'], ['bank', 'Settle to Capital One']];
const HOLDER_ONLY = 'Only Capital One account holders can be verified';
const day = iso => (iso ? new Date(iso).toLocaleDateString('en-GB', { day: 'numeric', month: 'short', year: 'numeric' }) : '—');

export default function Registry() {
  const now = useNow(5000);
  const { status, loading: booting } = useStatus(), gaps = useGaps('registry'), { post } = useApi();
  const badges = useData('badges'), atts = useData('attestations');
  const [target, setTarget] = useState(''), [pubkey, setPubkey] = useState(''), [name, setName] = useState('');
  const [kind, setKind] = useState('merchant'), [nessieRef, setNessieRef] = useState('');
  const [settle, setSettle] = useState('hack'), [settleAccount, setSettleAccount] = useState('');
  const [busy, setBusy] = useState(''); // 'issue' | the subject being revoked | 'enroll:<pubkey>'
  const [error, setError] = useState(null), [done, setDone] = useState(null); // error: { at, code, message }; done: the POST response

  const reg = status?.registry, auth = status?.authority;
  const configured = badges.data?.badges.filter(b => b.pubkey) ?? [];
  // Checked against the list on every render: a badge that left badges.json must not stay selected behind the select's back.
  const to = target === 'other' || configured.some(b => b.pubkey === target) ? target : configured[0]?.pubkey ?? 'other';
  const other = to === 'other' && !badges.loading, subject = to === 'other' ? pubkey.trim() : to; // other = the public key field is on screen
  const rows = atts.data?.attestations ?? [];
  const who = a => a.label ?? short(a.subject);
  const [link, setLink] = useState({ customer: '', account: '' }); // optional existing Nessie ids for the next Enroll
  const [enrolled, setEnrolled] = useState(null), [enrollError, setEnrollError] = useState(null); // { badge, customerId, accountId } | { badge, message }

  async function send(at, path, body) {
    setError(null); setDone(null); setBusy(at);
    try { setDone(await post(path, body)); if (at === 'issue') { setName(''); setNessieRef(''); setSettleAccount(''); } }
    catch (err) { setError({ at, code: err.code, message: err.message }); }
    finally { setBusy(''); }
  }
  function issue(e) {
    e.preventDefault(); setDone(null);
    const body = { pubkey: subject, name: name.trim(), kind, nessieRef: nessieRef.trim(), settleMode: settle };
    if (settle === 'bank') body.nessieAccountId = settleAccount.trim();
    if (!PUBKEY.test(body.pubkey)) return setError({ at: 'issue', code: 'invalid_pubkey', message: 'pubkey must be a base58 32-byte address' });
    if (!NAME.test(body.name)) return setError({ at: 'issue', code: 'invalid_name', message: 'name must be 1 to 32 printable ASCII characters' });
    if (!body.nessieRef) return setError({ at: 'issue', code: 'invalid_nessieRef', message: `${HOLDER_ONLY}: enter their Nessie customer or account id` });
    if (settle === 'bank' && !body.nessieAccountId) return setError({ at: 'issue', code: 'invalid_settleAccount', message: 'Settling to Capital One needs the Nessie account the dollars are deposited into' });
    send('issue', '/api/attestations', body);
  }
  async function enroll(b) {
    const body = { pubkey: b.pubkey };
    if (link.customer.trim()) body.nessieCustomerId = link.customer.trim();
    if (link.account.trim()) body.nessieAccountId = link.account.trim();
    setEnrolled(null); setEnrollError(null); setBusy(`enroll:${b.pubkey}`);
    try { setEnrolled({ badge: b, ...(await post('/api/enroll', body)) }); setLink({ customer: '', account: '' }); }
    catch (err) { setEnrollError({ badge: b, message: notBuilt(err) ? 'This backend has no /api/enroll yet.' : err.message }); }
    finally { setBusy(''); }
  }
  const revoke = a => confirm(`Revoke "${a.name}" for ${who(a)}?\n\nThis sends a transaction. Payments to this badge will show as REVOKED.`)
    && send(a.subject, '/api/attestations/revoke', { pubkey: a.subject });
  // A field error shows under its field; with the public key field off screen it falls through to the note above the table.
  const bad = field => (error?.at === 'issue' && error.code === `invalid_${field}` && (other || field !== 'pubkey') ? error.message : null);

  return (
    <Page title="Issuer" subtitle="Capital One verifies payees: bind a badge's key to a name and kind on chain, revoke it the same way, enroll badges at the bank.">
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
            <span>Kind</span>
            <select className="select" value={kind} onChange={e => { setKind(e.target.value); setError(null); }}>
              {KINDS.map(([k, label]) => <option key={k} value={k}>{label}</option>)}
            </select>
          </label>
          <label className="field">
            <span>Verified name · {name.trim().length}/32</span>
            <input className="input" value={name} onChange={e => setName(e.target.value)} maxLength={32} placeholder="MHacks Merch" autoComplete="off"
              aria-invalid={bad('name') ? 'true' : undefined} />
            {bad('name') && <small className="field-error" role="alert">{bad('name')}</small>}
          </label>
          <label className="field">
            <span>Nessie reference · required</span>
            <input className="input" value={nessieRef} onChange={e => setNessieRef(e.target.value)} placeholder="Nessie customer or account id" spellCheck="false" autoComplete="off"
              aria-invalid={bad('nessieRef') ? 'true' : undefined} required />
            {bad('nessieRef') ? <small className="field-error" role="alert">{bad('nessieRef')}</small>
              : <small className="muted small"><b>{HOLDER_ONLY}.</b> Hashed before it goes on chain: the attestation carries only the hash, the plain id stays in the backend.</small>}
          </label>
          <label className="field">
            <span>Settlement</span>
            <select className="select" value={settle} onChange={e => { setSettle(e.target.value); setError(null); }}>
              {SETTLE.map(([k, label]) => <option key={k} value={k}>{label}</option>)}
            </select>
            <small className="muted small">{settle === 'bank'
              ? "Payments go to Capital One's settlement wallet and are deposited as dollars into the Nessie account."
              : `Payments land in this badge's own wallet as ${status?.token?.symbol ?? 'HACK'}.`}</small>
          </label>
          {settle === 'bank' && (
            <label className="field">
              <span>Nessie account id · required</span>
              <input className="input" value={settleAccount} onChange={e => setSettleAccount(e.target.value)} placeholder="account the dollars land in" spellCheck="false" autoComplete="off"
                aria-invalid={bad('settleAccount') ? 'true' : undefined} required />
              {bad('settleAccount') ? <small className="field-error" role="alert">{bad('settleAccount')}</small>
                : <small className="muted small">Kept in the backend only, never on chain.</small>}
            </label>
          )}
          <div className="field">
            <span aria-hidden="true">&nbsp;</span>
            <button type="submit" className="btn btn--primary" disabled={Boolean(busy) || badges.loading}>{busy === 'issue' ? 'Sending…' : 'Issue attestation'}</button>
          </div>
        </form>
      </section>

      <section className="card" aria-label="Attestations">
        <div className="card-head"><h2>Attestations</h2><span className="muted">{!atts.data ? '' : `${rows.filter(a => a.status === 'verified').length} verified · ${rows.filter(a => a.status === 'expired').length} expired · ${rows.length} total`}</span></div>
        <div className="stack">
          {error && !['pubkey', 'name', 'wallet', 'nessieRef', 'settleAccount'].some(bad) && (
            <div className="note note--bad" role="alert"><b>{error.at === 'issue' ? 'Issue failed.' : 'Revoke failed.'}</b> {error.message}</div>
          )}
          {done && (
            <div className="note note--ok row" role="status">
              <b>{done.attestation?.status === 'revoked' ? 'Revoked' : 'Issued'} “{done.attestation?.name}”{done.attestation?.kind ? ` (${done.attestation.kind})` : ''}</b>
              <span>{done.attestation?.status === 'revoked' ? 'for' : 'to'} {done.attestation ? who(done.attestation) : '—'}{done.attestation?.status !== 'revoked' && done.attestation?.settleMode === 'bank' ? ', settling to Capital One' : ''}.</span>
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
                  <thead><tr><th>Badge</th><th>Verified name</th><th>Kind</th><th>Settles</th><th>Status</th><th>Action</th><th>Issued</th><th>Revoked</th><th>Expires</th></tr></thead>
                  <tbody>
                    {rows.map(a => (
                      <tr key={a.subject}>
                        <td>{a.label ?? 'Unknown wallet'}<br /><Pubkey value={a.subject} link /></td>
                        <td><b>{a.name ?? '—'}</b></td>
                        <td>{a.kind ? <span className="chip">{a.kind}</span> : <span className="muted">—</span>}</td>
                        {/* settleMode absent = a backend that predates it */}
                        <td>{a.settleMode === 'bank' ? <span className="chip chip--ok" title="Paid into Capital One's settlement wallet, deposited as dollars">Capital One</span>
                          : a.settleMode === 'hack' ? <span className="chip" title="Paid in HACK into its own wallet">HACK</span> : <span className="muted">—</span>}</td>
                        <td><StatusDot status={a.status} /></td>
                        <td>{a.status === 'verified' && (
                          <button type="button" className="btn btn--danger btn--small" disabled={Boolean(busy)} onClick={() => revoke(a)}
                            aria-label={`Revoke ${a.name} for ${who(a)}`}>{busy === a.subject ? 'Revoking…' : 'Revoke'}</button>
                        )}</td>
                        <td><ExplorerLink sig={a.issuedSig} />{a.issuedAt && <><br /><span className="muted small">{ago(a.issuedAt, now)} ago</span></>}</td>
                        <td><ExplorerLink sig={a.revokedSig} />{a.revokedAt && <><br /><span className="muted small">{ago(a.revokedAt, now)} ago</span></>}</td>
                        <td className={`num${a.status === 'expired' ? ' t-mute' : ''}`} title={a.expiresAt ?? undefined}>{day(a.expiresAt)}{a.status === 'expired' && <><br /><span className="small">re-issue to renew</span></>}</td>
                      </tr>
                    ))}
                  </tbody>
                </table>
              </div>
            )}
        </div>
      </section>

      <section className="card" aria-label="Bank enrollment">
        <div className="card-head"><h2>Bank enrollment</h2><span className="muted">A badge needs a Capital One (Nessie) account before it can top up {status?.token?.symbol ?? 'HACK'} or be verified.</span></div>
        <div className="stack">
          {enrollError && <div className="note note--bad" role="alert"><b>Enroll failed for {enrollError.badge.label ?? short(enrollError.badge.pubkey)}.</b> {enrollError.message}</div>}
          {enrolled && (
            <div className="note note--ok row" role="status">
              <b>Enrolled {enrolled.badge.label ?? short(enrolled.badge.pubkey)}.</b>
              <span>Account <span className="mono">{enrolled.accountId ?? '—'}</span>{enrolled.customerId && <> · customer <span className="mono">{enrolled.customerId}</span></>}</span>
            </div>
          )}
          <details>
            <summary className="label">Link existing Nessie ids instead of opening a new account</summary>
            <div className="grid issue" style={{ marginTop: 'var(--sp-2)' }}>
              <label className="field"><span>Customer id · optional</span>
                <input className="input" value={link.customer} onChange={e => setLink(l => ({ ...l, customer: e.target.value }))} spellCheck="false" autoComplete="off" /></label>
              <label className="field"><span>Account id · optional</span>
                <input className="input" value={link.account} onChange={e => setLink(l => ({ ...l, account: e.target.value }))} spellCheck="false" autoComplete="off" /></label>
            </div>
          </details>
          {badges.loading ? <Skeleton variant="row" lines={3} />
            : !configured.length ? <Empty title="No badges to enroll" hint="`server/config/badges.json` has no badge with a public key yet." />
            : (
              <div className="table-wrap">
                <table className="table">
                  <thead><tr><th>Badge</th><th>Bank</th><th>Action</th><th>Nessie account</th><th className="r">Balance</th></tr></thead>
                  <tbody>
                    {configured.map(b => {
                      const n = b.nessie, at = `enroll:${b.pubkey}`;
                      return (
                        <tr key={b.id}>
                          <td>{b.id} · {b.label}<br /><Pubkey value={b.pubkey} /></td>
                          {/* nessie undefined = this backend does not report enrollment yet; null = not enrolled */}
                          <td>{n === undefined ? <span className="muted">unknown</span> : n ? <span className="chip chip--ok">enrolled</span> : <span className="chip">not enrolled</span>}</td>
                          <td><button type="button" className="btn btn--small" disabled={Boolean(busy)} onClick={() => enroll(b)}
                            aria-label={`${n ? 'Re-enroll' : 'Enroll'} ${b.label}`}>{busy === at ? 'Enrolling…' : n ? 'Re-enroll' : 'Enroll'}</button></td>
                          <td className="hash">{n?.accountId ?? '—'}</td>
                          <td className="r num">{n ? usd(n.usdCents) : '—'}</td>
                        </tr>
                      );
                    })}
                  </tbody>
                </table>
              </div>
            )}
        </div>
      </section>
    </Page>
  );
}
