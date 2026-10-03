// Shared components for every page (PLAN 6.5). No dependencies: plain CSS classes from styles.css and inline SVG.
import { useEffect, useRef, useState } from 'react';

// ponytail: devnet explorer hardcoded (the whole project is devnet-only); read status.cluster if that ever changes.
const EXPLORER = 'https://explorer.solana.com';
export const explorerTx = sig => `${EXPLORER}/tx/${sig}?cluster=devnet`;
export const explorerAddr = addr => `${EXPLORER}/address/${addr}?cluster=devnet`;

export const short = (s, n = 4) => (!s ? '—' : s.length <= n * 2 + 1 ? s : `${s.slice(0, n)}…${s.slice(-n)}`);
export const fmt = (n, digits = 2) => (n == null ? '—' : Number(n).toLocaleString('en-US', { maximumFractionDigits: digits }));
export function ago(iso, now = Date.now()) {
  if (!iso) return '—';
  const s = Math.max(0, Math.round((now - Date.parse(iso)) / 1000));
  return s < 60 ? `${s}s` : s < 3600 ? `${Math.floor(s / 60)}m` : s < 86400 ? `${Math.floor(s / 3600)}h` : `${Math.floor(s / 86400)}d`;
}
/** Re-renders the caller every `ms` and returns Date.now(): keeps relative times ticking. */
export function useNow(ms = 1000) {
  const [now, setNow] = useState(Date.now);
  useEffect(() => { const id = setInterval(() => setNow(Date.now()), ms); return () => clearInterval(id); }, [ms]);
  return now;
}
/** [copied, copy(text)]: copies to the clipboard and flips `copied` for 1.2 s. */
export function useCopy() {
  const [copied, setCopied] = useState(false), t = useRef(0);
  useEffect(() => () => clearTimeout(t.current), []);
  return [copied, text => {
    navigator.clipboard?.writeText(text).catch(() => {});
    setCopied(true); clearTimeout(t.current); t.current = setTimeout(() => setCopied(false), 1200);
  }];
}
// `backticks` in gap/empty text become <code>, so commands and file names stand out.
const rich = text => String(text ?? '').split('`').map((t, i) => (i % 2 ? <code key={i}>{t}</code> : t));

export function Page({ title, subtitle, actions, children }) {
  return (
    <main className="page">
      <header className="page-head">
        <div>
          <h1>{title}</h1>
          {subtitle && <p className="page-sub">{subtitle}</p>}
        </div>
        {actions && <div className="page-actions">{actions}</div>}
      </header>
      {children}
    </main>
  );
}

/** variant: 'block' (one w×h box) | 'text' (`lines` text lines) | 'row' (`lines` table rows) | 'card' (a stat-card shape). */
export function Skeleton({ w, h, lines = 1, variant = 'block' }) {
  if (variant === 'block') return <i className="skel" style={{ width: w, height: h }} aria-hidden="true" />;
  if (variant === 'card') return (
    <div className="card skel-card" style={{ width: w, height: h }} aria-hidden="true">
      <i className="skel" style={{ width: '45%', height: '.8rem' }} />
      <i className="skel" style={{ width: '70%', height: '2.6rem' }} />
      <i className="skel" style={{ width: '55%', height: '.8rem' }} />
    </div>
  );
  return (
    <div className={`skel-stack skel-stack--${variant}`} style={{ width: w }} aria-hidden="true">
      {Array.from({ length: lines }, (_, i) => (
        <i key={i} className="skel" style={{ height: h, width: variant === 'text' && lines > 1 && i === lines - 1 ? '60%' : undefined }} />
      ))}
    </div>
  );
}

/** tone: 'ok' | 'warn' | 'bad' tints the value. */
export function StatTile({ label, value, sub, tone }) {
  return (
    <div className="card stat">
      <div className="label">{label}</div>
      <div className={`stat-value num${tone ? ` t-${tone}` : ''}`} style={{ '--n': String(value).length }}>{value}</div>
      {sub && <div className="stat-sub">{sub}</div>}
    </div>
  );
}

/** Middle-truncated address. Click copies the full value. `link` adds an explorer (address) link. Null value renders "—". */
export function Pubkey({ value, link, chars = 4 }) {
  const [copied, copy] = useCopy();
  if (!value) return <span className="hash">—</span>;
  return (
    <span className="pubkey">
      <button type="button" className="hash copy" title={`${value} (click to copy)`} aria-label={`Copy address ${value}`}
        onClick={e => { e.stopPropagation(); copy(value); }}>{copied ? 'copied' : short(value, chars)}</button>
      {link && <a className="ext" href={explorerAddr(value)} target="_blank" rel="noreferrer" aria-label="Open address in Solana Explorer">↗</a>}
    </span>
  );
}

// rejected = the human caught the tampered charge (the defence held) = green. signed = the tampered transfer landed = red.
const TONE = {
  verified: 'ok', live: 'ok', rejected: 'ok', unverified: 'warn', pending: 'warn', reconnecting: 'warn', backfilling: 'warn',
  revoked: 'bad', signed: 'bad', offline: 'bad', expired: 'mute', idle: 'mute',
};
/** status: verified | unverified | revoked | pending | signed | rejected | expired (also live | reconnecting | idle | offline). */
export function StatusDot({ status, label }) {
  const text = label ?? status;
  return <span className={`status s-${status} t-${TONE[status] ?? 'mute'}`}><i className="dot" aria-hidden="true" />{text && <span>{text}</span>}</span>;
}

/** gap: one entry of /api/status.gaps. blocker = full panel with the fix, warn = strip, info = muted line. */
export function GapCard({ gap }) {
  if (!gap) return null;
  const sev = ['blocker', 'warn', 'info'].includes(gap.severity) ? gap.severity : 'warn';
  return (
    <div className={`gap gap--${sev}`} role={sev === 'blocker' ? 'alert' : 'note'}>
      <span className="gap-mark" aria-hidden="true">{sev === 'info' ? 'i' : '!'}</span>
      <div className="gap-body">
        <div className="gap-title">{rich(gap.title)}</div>
        {gap.fix && <div className="gap-fix">{rich(gap.fix)}</div>}
      </div>
      {gap.badgeGap && <span className="chip" title="Waiting on the badge hardware. grep the repo for this marker.">BADGE-GAP({gap.badgeGap})</span>}
    </div>
  );
}

export function Empty({ icon = '∅', title, hint }) {
  return (
    <div className="empty">
      <div className="empty-icon" aria-hidden="true">{icon}</div>
      <div className="empty-title">{title}</div>
      {hint && <div className="empty-hint">{rich(hint)}</div>}
    </div>
  );
}

/** Two-segment switch: [labelOff | labelOn]. `label` names it for screen readers. */
export function Toggle({ on, onChange, labelOn = 'ON', labelOff = 'OFF', label }) {
  return (
    <button type="button" className="toggle" role="switch" aria-checked={on}
      aria-label={`${label ? `${label}: ` : ''}${on ? labelOn : labelOff}. Switch to ${on ? labelOff : labelOn}`} onClick={() => onChange(!on)}>
      <span>{labelOff}</span><span>{labelOn}</span>
    </button>
  );
}

export function Sparkline({ series = [], field = 'n', w = 120, h = 28 }) {
  const max = Math.max(1, ...series.map(d => d[field])), dx = w / Math.max(1, series.length - 1);
  return (
    <svg className="spark" width={w} height={h} viewBox={`0 0 ${w} ${h}`} role="img" aria-label={`${field} trend`}>
      <polyline fill="none" stroke="var(--accent)" strokeWidth="2" strokeLinejoin="round" strokeLinecap="round"
        points={series.map((d, i) => `${(i * dx).toFixed(1)},${(h - 2 - (d[field] / max) * (h - 4)).toFixed(1)}`).join(' ')} />
    </svg>
  );
}

/** One bar per bucket; with field 'n' the verified share of each bar is green. `highlight` = index at full strength (default: last). `fluid` = 100% width. */
export function Bars({ series = [], field = 'n', w = 520, h = 46, highlight = series.length - 1, fluid }) {
  const max = Math.max(1, ...series.map(d => d[field])), bw = w / Math.max(1, series.length);
  return (
    <svg className="bars" width={fluid ? '100%' : w} height={h} viewBox={`0 0 ${w} ${h}`} preserveAspectRatio="none" role="img" aria-label="payments per minute">
      {series.map((d, i) => {
        const bh = Math.max(1.5, (d[field] / max) * (h - 2)), vh = field === 'n' && d.n ? bh * (d.verifiedN / d.n) : 0;
        return (
          <g key={d.t ?? i} opacity={i === highlight ? 1 : 0.7}>
            <rect x={i * bw + 1} y={h - bh} width={Math.max(1, bw - 2)} height={bh} fill="var(--ink-3)" />
            {vh > 0 && <rect x={i * bw + 1} y={h - vh} width={Math.max(1, bw - 2)} height={vh} fill="var(--ok)" />}
          </g>
        );
      })}
    </svg>
  );
}

/** Link to the transaction on the devnet explorer. Default text is the shortened signature. */
export function ExplorerLink({ sig, children }) {
  if (!sig) return <span className="hash">—</span>;
  return <a className="ext-link" href={explorerTx(sig)} target="_blank" rel="noreferrer" title={sig}>{children ?? short(sig, 6)}<span aria-hidden="true"> ↗</span></a>;
}

/** Formats raw base units (the API's `amountRaw` string) with thousands separators. */
export function Amount({ raw, decimals = 0, symbol }) {
  // ponytail: Number is exact to 2^53 raw units; HACK amounts are nowhere near that.
  const text = raw == null ? '—' : (Number(raw) / 10 ** decimals).toLocaleString('en-US', { maximumFractionDigits: decimals });
  return <span className="num amount">{text}{symbol && <small> {symbol}</small>}</span>;
}
