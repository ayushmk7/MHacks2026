// Data layer. One module, two transports: DEMO (in-memory fixtures + ticker) and LIVE (/api/* + one SSE stream).
// Pages only use the hooks below and never know which mode is active.
import { createContext, useCallback, useContext, useEffect, useRef, useState } from 'react';
import { bumpStats, demoGet, demoPost, startDemoTicker } from './demo.js';

const Ctx = createContext(null);
const ls = {
  get: k => { try { return localStorage.getItem(k); } catch { return null; } },
  set: (k, v) => { try { localStorage.setItem(k, v); } catch { /* private mode: just don't persist */ } },
};
const sleep = ms => new Promise(r => setTimeout(r, ms));
const keyOf = (resource, params) => {
  const q = new URLSearchParams(params ?? {}).toString();
  return q ? `${resource}?${q}` : resource;
};
const offline = () => Object.assign(new Error('Backend unreachable at 127.0.0.1:8787. Start it with `npm run server`, or switch to DEMO.'), { code: 'offline', status: 0 });

// Errors are always Error objects carrying the server's { code, message } (PLAN 4.4), plus `status` and `gap`.
async function http(path, init) {
  let res;
  try { res = await fetch(path, init); } catch { throw offline(); }
  const body = await res.json().catch(() => null);
  if (res.ok && body) return body;
  if (body?.error) throw Object.assign(new Error(body.error.message), { code: body.error.code, status: res.status, gap: body.gap ?? body.error.gap });
  // No JSON. A 502 is the Vite proxy finding nothing on 8787; a 2xx or 404 is a static host with no /api at all. Either way: no backend.
  if (res.ok || res.status === 404 || res.status === 502) throw offline();
  throw Object.assign(new Error(`The server answered HTTP ${res.status} without a JSON error body. Check the server log.`), { code: 'bad_response', status: res.status });
}
// The demo delays make skeleton and "sending" states visible, and keep both modes on the same async path.
const transport = {
  demo: {
    get: async (resource, params) => { await sleep(350); return demoGet(resource, params); },
    post: async (path, body) => { await sleep(700); return demoPost(path, body); },
  },
  live: {
    get: (resource, params) => http('/api/' + keyOf(resource, params)),
    post: (path, body) => http(path, { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(body) }),
  },
};

export function DataProvider({ children }) {
  const [mode, setModeState] = useState(() => (ls.get('badgepay.mode') === 'live' ? 'live' : 'demo'));
  const [theme, setThemeState] = useState(() => (document.documentElement.dataset.theme === 'light' ? 'light' : 'dark'));
  const [store, setStore] = useState({}); // key -> { data, error, loading }
  const [mutating, setMutating] = useState(0);
  const modeRef = useRef(mode);
  const active = useRef(new Map());   // key -> [resource, params]; ponytail: never pruned, a visited page stays warm
  const inflight = useRef(new Map()); // ponytail: a refetch that lands during an in-flight GET reuses that GET
  const due = useRef(new Set()), timer = useRef(0), wasDown = useRef(false);
  const [streamNo, setStreamNo] = useState(0); // bumped to open a fresh EventSource after the browser gave up on one

  const load = useCallback((resource, params) => {
    const key = keyOf(resource, params), m = modeRef.current, id = m + key;
    active.current.set(key, [resource, params]);
    if (inflight.current.has(id)) return inflight.current.get(id);
    const put = patch => m === modeRef.current && setStore(s => ({ ...s, [key]: { ...s[key], ...patch } }));
    put({ loading: true });
    const p = transport[m].get(resource, params)
      .then(data => put({ data, error: null, loading: false }), error => put({ error, loading: false })) // a failure keeps the last data
      .finally(() => inflight.current.delete(id));
    inflight.current.set(id, p);
    return p;
  }, []);

  const reloadAll = useCallback(() => Promise.all([...active.current.values()].map(([r, p]) => load(r, p))), [load]);

  // Debounced 250 ms so a burst of events causes one GET per resource. '*' = everything mounted.
  const refetch = useCallback((...resources) => {
    resources.forEach(r => due.current.add(r));
    clearTimeout(timer.current);
    timer.current = setTimeout(() => {
      const d = due.current; due.current = new Set();
      for (const [r, p] of active.current.values()) if (d.has('*') || d.has(r)) load(r, p);
    }, 250);
  }, [load]);

  // The single entry point for realtime: SSE messages in LIVE, the ticker in DEMO. Same shape: { type, data }.
  const onEvent = useCallback(({ type, data }) => {
    if (type === 'payment') {
      // ponytail: only the param-less 'payments' list gets the prepend; other payment queries just refetch.
      setStore(s => {
        const list = s.payments?.data?.payments;
        if (!list || list.some(p => p.signature === data.signature)) return s;
        // Sorted, not just prepended: the server's backfill can deliver a block that is older than the current head.
        const merged = [data, ...list].sort((a, b) => b.blockTime.localeCompare(a.blockTime) || b.slot - a.slot).slice(0, 200);
        const next = { ...s, payments: { ...s.payments, data: { payments: merged } } };
        if (s.stats?.data) next.stats = { ...s.stats, data: bumpStats(s.stats.data, data) };
        return next;
      });
      refetch('stats', 'stats/db', 'badges');
    } else if (type === 'attestation') refetch('attestations', 'badges');
    else if (type === 'attack') refetch('attacks');
    else if (type === 'status') refetch('*');
  }, [refetch]);

  const setMode = useCallback(m => {
    if (m === modeRef.current) return;
    modeRef.current = m; ls.set('badgepay.mode', m);
    wasDown.current = false;
    setStore({}); // demo data is never shown under a LIVE label, and vice versa
    setModeState(m);
  }, []);
  const setTheme = useCallback(t => {
    document.documentElement.dataset.theme = t; ls.set('badgepay.theme', t);
    setThemeState(t);
  }, []);

  // Status: poll every 5 s. It doubles as the backend heartbeat in LIVE.
  useEffect(() => {
    load('status');
    const id = setInterval(() => load('status'), 5000);
    return () => clearInterval(id);
  }, [mode, load]);

  const st = store.status, up = mode === 'demo' || Boolean(st?.data && !st.error);
  useEffect(() => { // backend came back: catch up on everything that was missed
    if (st?.error) wasDown.current = true;
    else if (st?.data && wasDown.current) { wasDown.current = false; reloadAll(); }
  }, [st, reloadAll]);

  useEffect(() => {
    if (mode === 'demo') return startDemoTicker(onEvent);
    if (!up) return; // no stream while the backend is unreachable: no reconnect spam in the console
    const es = new EventSource('/api/events');
    let dropped = streamNo > 0, again = 0;
    es.onmessage = e => { try { onEvent(JSON.parse(e.data)); } catch { /* not ours */ } };
    // A retry answered with a 5xx (server mid-restart) closes an EventSource for good, and the status poll can miss a short outage.
    es.onerror = () => { dropped = true; if (es.readyState === EventSource.CLOSED) again = setTimeout(() => setStreamNo(n => n + 1), 3000); };
    es.onopen = () => { if (dropped) { dropped = false; refetch('*'); } };
    return () => { clearTimeout(again); es.close(); };
  }, [mode, up, streamNo, onEvent, refetch]);

  const get = useCallback((resource, params) => transport[modeRef.current].get(resource, params), []);
  const post = useCallback(async (path, body) => {
    setMutating(n => n + 1);
    try {
      const out = await transport[modeRef.current].post(path, body);
      refetch(path.includes('/attacks') ? 'attacks' : 'attestations', 'badges'); // don't depend on SSE alone
      return out;
    } finally { setMutating(n => n - 1); }
  }, [refetch]);

  return <Ctx.Provider value={{ mode, setMode, theme, setTheme, store, load, reloadAll, get, post, mutating: mutating > 0 }}>{children}</Ctx.Provider>;
}

const view = s => ({ data: s?.data ?? null, loading: !s?.data && !s?.error, error: s?.error ?? null });

/** { mode:'demo'|'live', setMode(mode), toggle() }. Persisted at localStorage['badgepay.mode'], default 'demo'. */
export function useMode() {
  const { mode, setMode } = useContext(Ctx);
  return { mode, setMode, toggle: () => setMode(mode === 'demo' ? 'live' : 'demo') };
}

/** { theme:'dark'|'light', setTheme(theme), toggle() }. Persisted at localStorage['badgepay.theme']. */
export function useTheme() {
  const { theme, setTheme } = useContext(Ctx);
  return { theme, setTheme, toggle: () => setTheme(theme === 'dark' ? 'light' : 'dark') };
}

/** { status, loading, error, retry() }. `status` is the /api/status body (or the demo one). `error` set = backend unreachable. */
export function useStatus() {
  const { store, reloadAll } = useContext(Ctx), v = view(store.status);
  return { status: v.data, loading: v.loading, error: v.error, retry: reloadAll };
}

/**
 * Gaps from /api/status that the page `page` ('feed'|'badges'|'registry'|'attack') must render itself.
 * App-wide blockers (database, RPC) are left out: the shell already shows those under the top bar.
 */
export function useGaps(page) {
  const { status } = useStatus();
  return (status?.gaps ?? []).filter(g => g.pages?.includes(page) && !isGlobalGap(g));
}
export const isGlobalGap = g => g.severity === 'blocker' && (g.pages?.length ?? 0) >= 4;

/**
 * GET a resource: 'payments' | 'stats' | 'stats/db' | 'badges' | 'attestations' | 'attacks' (params become the query string).
 * Returns { data, loading, error, refetch }. `data` is the response body exactly as in the API contract.
 * loading = nothing to show yet (render skeletons). error with data = stale data, keep rendering it.
 */
export function useData(resource, params) {
  const { store, load, mode } = useContext(Ctx);
  const key = keyOf(resource, params);
  useEffect(() => { load(resource, params); }, [key, mode, load]); // eslint-disable-line react-hooks/exhaustive-deps
  return { ...view(store[key]), refetch: () => load(resource, params) };
}

/**
 * { post(path, body) -> Promise<json>, get(resource, params) -> Promise<json>, mutating }.
 * post rejects with an Error carrying { code, message, status, gap } from the server's error shape.
 * `get` is a one-off, uncached read (used for "load older").
 */
export function useApi() {
  const { post, get, mutating } = useContext(Ctx);
  return { post, get, mutating };
}
