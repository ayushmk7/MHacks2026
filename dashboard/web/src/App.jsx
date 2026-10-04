// Shell: top bar, hash routing, DEMO/LIVE + theme switches, offline and blocker banners.
import { Component, useEffect, useState } from 'react';
import { isGlobalGap, useMode, useStatus, useTheme } from './data.jsx';
import { Empty, GapCard, Toggle } from './ui.jsx';

// Eager glob: a page file that does not exist yet is simply absent from this map, so the app still boots.
const Pages = import.meta.glob('./pages/*.jsx', { eager: true });
const ROUTES = [['feed', 'FEED', 'Feed'], ['badges', 'BADGES', 'Badges'], ['registry', 'ISSUER', 'Registry'], ['attack', 'ATTACK', 'Attack']];
const routeOf = () => {
  const r = location.hash.replace(/^#\/?/, '');
  return ROUTES.some(([id]) => id === r) ? r : 'feed';
};
const INGEST = { live: ['LIVE', 'ok'], reconnecting: ['RECONNECTING', 'warn'], backfilling: ['BACKFILLING', 'warn'] };

// A page that throws shows a placard instead of blanking the panel mid-demo.
class Boundary extends Component {
  state = { error: null };
  static getDerivedStateFromError(error) { return { error }; }
  render() {
    if (!this.state.error) return this.props.children;
    return <main className="page"><Empty icon="×" title="This page crashed" hint={String(this.state.error?.message ?? this.state.error)} /></main>;
  }
}

export default function App() {
  const [route, setRoute] = useState(routeOf);
  const [retrying, setRetrying] = useState(false);
  const { mode, setMode } = useMode(), { theme, setTheme } = useTheme(), { status, loading, error, retry } = useStatus();
  useEffect(() => {
    const onHash = () => setRoute(routeOf());
    addEventListener('hashchange', onHash);
    return () => removeEventListener('hashchange', onHash);
  }, []);

  const live = mode === 'live';
  const [pill, tone] = !live ? ['DEMO', 'warn'] : error ? [error.code === 'offline' ? 'OFFLINE' : 'ERROR', 'bad'] : loading ? ['CONNECTING', 'mute'] : INGEST[status.ingest?.state] ?? ['IDLE', 'mute'];
  const file = ROUTES.find(([id]) => id === route)[2];
  const Page = Pages[`./pages/${file}.jsx`]?.default;

  return (
    <>
      <div className="top">
        <header className="bar">
          <a className="brand" href="#/feed" aria-label="BadgePay, go to feed">BADGE<i>·</i>PAY</a>
          <nav className="nav" aria-label="Pages">
            {ROUTES.map(([id, name]) => <a key={id} href={`#/${id}`} aria-current={id === route ? 'page' : undefined}>{name}</a>)}
          </nav>
          <div className="bar-right">
            <span className={`pill t-${tone}`} role="status" title={live ? 'Ingest state from /api/status' : 'Fixture data, no network'}><i className="dot" aria-hidden="true" />{pill}</span>
            <Toggle label="Data mode" on={live} onChange={on => setMode(on ? 'live' : 'demo')} labelOn="LIVE" labelOff="DEMO" />
            <Toggle label="Theme" on={theme === 'dark'} onChange={on => setTheme(on ? 'dark' : 'light')} labelOn="DARK" labelOff="LIGHT" />
          </div>
        </header>
        {live && error && (
          <div className="banner" role="alert">
            <span className="gap-mark" aria-hidden="true">!</span>
            <div className="gap-body">
              {error.code === 'offline' ? <><b>Backend offline.</b> Nothing is answering on 127.0.0.1:8787. Start it with <code>npm run server</code>, or switch to DEMO.</>
                : <><b>Backend error.</b> {error.message}</>}
            </div>
            <button type="button" className="btn" disabled={retrying}
              onClick={() => { setRetrying(true); retry().finally(() => setRetrying(false)); }}>{retrying ? 'Retrying…' : 'Retry'}</button>
          </div>
        )}
      </div>
      {/* App-wide blockers (database, RPC). Page-specific gaps are rendered by each page via useGaps(page). */}
      {live && !error && status?.gaps?.some(isGlobalGap) && (
        <div className="banners">{status.gaps.filter(isGlobalGap).map(g => <GapCard key={g.code} gap={g} />)}</div>
      )}
      <Boundary key={route + mode}>
        {Page ? <Page /> : (
          <main className="page">
            <Empty icon="…" title="Page not built yet" hint={`\`web/src/pages/${file}.jsx\` does not exist yet. The rest of the panel works without it.`} />
          </main>
        )}
      </Boundary>
    </>
  );
}
