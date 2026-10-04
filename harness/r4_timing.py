#!/usr/bin/env python3
"""R4 presence timing: laptop side. Stdlib only.

  python harness/r4_timing.py serve [--port 8790]           # collect runs the badges upload
  python harness/r4_timing.py push --host <badge-ip> --token <code> [--listener URL]
  python harness/r4_timing.py push --out <dir> [--listener URL]      # stage only, push nothing
  python harness/r4_timing.py report [DIR ...]              # tables + proposed deadlines
  python harness/r4_timing.py --check                       # self-test of the statistics

`push` writes this laptop's address into harness/apps/r4-presence/config.lua (in a temporary
copy), adds lib/vk.lua, installs it and launches it (harness/badge_app.py). Push it to both badges. On either badge, SELECT runs
200 CHAL -> PROOF exchanges against the other one.

`serve` writes every uploaded line to harness/results/<UTC timestamp>-r4/runs.jsonl (deduped
by run id) and prints each run's summary. Runs made with Wi-Fi off are saved on the badge and
uploaded once Wi-Fi is back (DOWN on the badge, or automatically after the next run).

`report` reads runs.jsonl from the given folders (default: every *-r4 folder) and writes
table.md next to the newest: p50 / p95 / max and loss per Wi-Fi state and payee key location,
and PRESENCE_DEADLINE_MS = p95 x 1.5 (00 §4), rounded up to 10 ms, per key location, taking the
worse Wi-Fi state so the deadline holds in both.
"""

import argparse
import json
import math
import sys
from datetime import datetime, timezone
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

from badge_app import add_push_args, check_push_args, laptop_ip, push as push_app

HERE = Path(__file__).resolve().parent
RESULTS = HERE / "results"
MARGIN = 1.5


def pct(sorted_values, q):
    """Nearest-rank percentile, the same rule the badge app uses."""
    if not sorted_values:
        return None
    return sorted_values[max(1, math.ceil(q * len(sorted_values))) - 1]


def stats(values):
    s = sorted(values)
    return {"n": len(s), "p50": pct(s, 0.50), "p95": pct(s, 0.95), "max": s[-1] if s else None}


def deadline(p95):
    return None if p95 == math.inf else int(math.ceil(p95 * MARGIN / 10.0) * 10)


def all_times(r):
    """Every exchange of a run: received, late at their real time, never-arrived as inf. Without
    the last two a slow tail that times out would vanish from p95 into the loss count."""
    return r["rtt_ms"] + r["late_ms"] + [math.inf] * (r["lost"] - len(r["late_ms"]))


def ms(v):
    return "lost" if v == math.inf else str(v)


def run_line(r):
    s = stats(all_times(r))
    return (f"run {r['id']} wifi={r['wifi']} ch={r['channel']} payer={r['badge'][:8]}({r['badge_loc']})"
            f" payee={r['peer_name']}({r['peer_loc']}) n={s['n']} p50={ms(s['p50'])} p95={ms(s['p95'])} max={ms(s['max'])}"
            f" lost={r['lost']}/{r['runs']} late={len(r['late_ms'])} bad_sig={r['bad_sig']} signer={r['signer']}")


# ── serve ─────────────────────────────────────────────────────────────────────────────────────
def serve(port, out_dir=None):
    out_dir = Path(out_dir) if out_dir else RESULTS / (datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ") + "-r4")
    out_dir.mkdir(parents=True, exist_ok=True)
    out = out_dir / "runs.jsonl"
    seen = set()

    class Handler(BaseHTTPRequestHandler):
        def reply(self, code, obj):
            body = json.dumps(obj).encode()
            self.send_response(code)
            self.send_header("content-type", "application/json")
            self.send_header("content-length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        def do_GET(self):
            if self.path.split("?")[0] == "/health":
                return self.reply(200, {"ok": True})
            self.reply(404, {"error": "not_found"})

        def do_POST(self):
            if self.path.split("?")[0] != "/r4/result":
                return self.reply(404, {"error": "not_found"})
            text = self.rfile.read(int(self.headers.get("content-length") or 0)).decode("utf-8", "replace")
            added = 0
            with out.open("a") as f:
                for line in text.splitlines():
                    if not line.strip():
                        continue
                    try:
                        rec = json.loads(line)
                    except json.JSONDecodeError:
                        print(f"[r4] {self.client_address[0]} sent a bad line: {line[:80]}")
                        continue
                    # Runs are uploaded again with every later upload; keep the first copy. A payee
                    # record is a growing snapshot of one app launch; append each, report keeps the last.
                    if rec.get("kind") == "run":
                        if rec.get("id") in seen:
                            continue
                        seen.add(rec.get("id"))
                    f.write(json.dumps(rec) + "\n")
                    added += 1
                    if rec.get("kind") == "run":
                        print("[r4] " + run_line(rec))
                    else:
                        s = stats(rec.get("sign_ms", []))
                        print(f"[r4] payee {rec['badge'][:8]}({rec['badge_loc']}) answered={rec['answered']}"
                              f" sign p50={s['p50']} p95={s['p95']} max={s['max']} fail={rec['sign_fail']}")
            self.reply(200, {"ok": True, "added": added})

        def log_message(self, fmt, *args):
            pass

    print(f"Collecting R4 runs on http://{laptop_ip()}:{port}/r4/result -> {out}  (Ctrl+C to stop)")
    try:
        ThreadingHTTPServer(("0.0.0.0", port), Handler).serve_forever()
    except KeyboardInterrupt:
        pass
    if out.exists():
        print(f"\nSaved to {out}. Now: python3 harness/r4_timing.py report {out_dir}")


# ── push ──────────────────────────────────────────────────────────────────────────────────────
def push(args):
    listener = args.listener or f"http://{laptop_ip()}:{args.port}"
    sys.exit(push_app("r4-presence", {"listener": listener}, args))


# ── report ────────────────────────────────────────────────────────────────────────────────────
def load(dirs):
    """Runs deduped by id (first copy wins); payee records by id, latest snapshot wins."""
    runs, payees = {}, {}
    for d in dirs:
        f = Path(d) / "runs.jsonl"
        if not f.exists():
            continue
        for line in f.read_text().splitlines():
            if line.strip():
                rec = json.loads(line)
                if rec.get("kind") == "payee":
                    payees[rec["id"]] = rec
                else:
                    runs.setdefault(rec.get("id"), rec)
    return list(runs.values()) + list(payees.values())


def summarize(records):
    """Pools runs by (payee key location, Wi-Fi state). Returns rows and per-key deadlines."""
    groups = {}
    for r in records:
        if r.get("kind") != "run":
            continue
        g = groups.setdefault((r["peer_loc"], r["wifi"]), {"rtt": [], "sent": 0, "lost": 0, "late": 0,
                                                           "send_fail": 0, "bad_sig": 0, "runs": 0, "signer": set()})
        g["rtt"] += all_times(r)
        g["sent"] += r["runs"]
        g["lost"] += r["lost"]
        g["late"] += len(r["late_ms"])
        g["send_fail"] += r["send_fail"]
        g["bad_sig"] += r["bad_sig"]
        g["runs"] += 1
        g["signer"].add(r["signer"])
    rows = []
    for (loc, wifi), g in sorted(groups.items()):
        s = stats(g["rtt"])
        rows.append({"key": loc, "wifi": wifi, **s, "sent": g["sent"], "lost": g["lost"],
                     "loss_pct": 100.0 * g["lost"] / g["sent"] if g["sent"] else 0.0, "late": g["late"],
                     "send_fail": g["send_fail"], "bad_sig": g["bad_sig"], "runs": g["runs"],
                     "signer": "/".join(sorted(g["signer"]))})
    deadlines = {}
    for row in rows:
        if row["p95"] is None:
            continue
        cur = deadlines.get(row["key"])
        if cur is None or row["p95"] > cur["p95"]:
            deadlines[row["key"]] = {"p95": row["p95"], "wifi": row["wifi"], "deadline_ms": deadline(row["p95"])}
    # inf > any number, so a group with >5% never-arrived PROOFs wins and yields no deadline.
    return rows, deadlines


def report(dirs):
    dirs = [Path(d) for d in dirs] or sorted(RESULTS.glob("*-r4"))
    records = load(dirs)
    if not records:
        sys.exit("no runs.jsonl with records in " + ", ".join(map(str, dirs)) + " - run `serve` and upload first")
    rows, deadlines = summarize(records)
    payees = [r for r in records if r.get("kind") == "payee"]

    lines = ["# R4 presence timing", "",
             f"Generated {datetime.now(timezone.utc):%Y-%m-%d %H:%M} UTC from {', '.join(d.name for d in dirs)}.",
             "CHAL -> signed PROOF round trip on the payer, `badge.millis()`, nonce creation to PROOF receipt.",
             "Percentiles cover every exchange sent: late PROOFs at their real time, missing ones as `lost`.",
             "Grouped by the payee's key location (where the signature is made) and Wi-Fi state.", "",
             "| Payee key | Wi-Fi | Runs | Exchanges | p50 ms | p95 ms | max ms | Lost | Loss % | Late | Bad sig | Signer |",
             "| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |"]
    for r in rows:
        lines.append(f"| {r['key']} | {r['wifi']} | {r['runs']} | {r['sent']} | {ms(r['p50'])} | {ms(r['p95'])} | {ms(r['max'])}"
                     f" | {r['lost']} | {r['loss_pct']:.1f} | {r['late']} | {r['bad_sig']} | {r['signer']} |")
    lines += ["", "## Proposed PRESENCE_DEADLINE_MS (p95 x 1.5, worse Wi-Fi state, rounded up to 10 ms)", "",
              "| Key | Worst p95 ms | Measured with Wi-Fi | Deadline ms |", "| --- | --- | --- | --- |"]
    for loc in ("nvs", "se050"):
        d = deadlines.get(loc)
        if not d:
            lines.append(f"| {loc} | not measured | - | - |")
        elif d["deadline_ms"] is None:
            lines.append(f"| {loc} | lost | {d['wifi']} | none: over 5% of PROOFs never arrived, fix the link first |")
        else:
            lines.append(f"| {loc} | {d['p95']} | {d['wifi']} | **{d['deadline_ms']}** |")
    if payees:
        lines += ["", "## Payee signing time (inside the round trip)", "",
                  "| Payee | Key | Wi-Fi | Answered | sign p50 ms | p95 ms | max ms | Failures |",
                  "| --- | --- | --- | --- | --- | --- | --- | --- |"]
        for p in payees:
            s = stats(p["sign_ms"])
            lines.append(f"| {p['badge'][:8]} | {p['badge_loc']} | {p['wifi']} | {p['answered']} | {s['p50']}"
                         f" | {s['p95']} | {s['max']} | {p['sign_fail']} |")
    lines += ["", "## Runs", ""] + [f"- {run_line(r)}" for r in records if r.get("kind") == "run"]
    out = dirs[-1] / "table.md"
    out.write_text("\n".join(lines) + "\n")
    print("\n".join(lines))
    print(f"\nWrote {out}")


def check():
    assert pct([], 0.5) is None
    assert pct([7], 0.95) == 7
    v = list(range(1, 201))                     # 1..200
    assert pct(v, 0.50) == 100 and pct(v, 0.95) == 190, (pct(v, 0.5), pct(v, 0.95))
    assert deadline(190) == 290 and deadline(100) == 150 and deadline(101) == 160
    run = lambda loc, wifi, rtt, lost=0: {"kind": "run", "id": f"{loc}{wifi}{len(rtt)}{lost}", "peer_loc": loc,
                                          "wifi": wifi, "rtt_ms": rtt, "runs": len(rtt) + lost, "lost": lost,
                                          "late_ms": [], "send_fail": 0, "bad_sig": 0, "signer": "stub"}
    rows, d = summarize([run("nvs", "on", v), run("nvs", "off", [10] * 200), run("se050", "off", [300] * 100)])
    assert d["nvs"] == {"p95": 190, "wifi": "on", "deadline_ms": 290}, d
    assert d["se050"]["deadline_ms"] == 450
    # 170 fast + 10 late at 1200 ms + 20 never arrived (10%): p95 and max are lost, no deadline
    slow = run("se050", "on", [100] * 170, lost=30)
    slow["late_ms"] = [1200] * 10
    rows, d = summarize([slow])
    assert rows[0]["loss_pct"] == 15.0 and rows[0]["p95"] == math.inf and rows[0]["max"] == math.inf
    assert d["se050"]["deadline_ms"] is None
    slow = run("se050", "on", [100] * 185, lost=15)
    slow["late_ms"] = [1200] * 14                # 1 never arrived: p95 = 1200, not 100
    assert stats(all_times(slow))["p95"] == 1200 and summarize([slow])[1]["se050"]["deadline_ms"] == 1800
    print("r4_timing self-test ok")


def main():
    if "--check" in sys.argv[1:]:
        return check()
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="command", required=True)
    s = sub.add_parser("serve", help="collect runs uploaded by the badges")
    s.add_argument("--port", type=int, default=8790)
    s.add_argument("--out", help="result folder (default harness/results/<UTC timestamp>-r4)")
    p = sub.add_parser("push", help="install and launch the R4 app on a badge over Wi-Fi")
    add_push_args(p, "collector base URL (default http://<this laptop>:8790)", 8790)
    r = sub.add_parser("report", help="tables and proposed deadlines from collected runs")
    r.add_argument("dirs", nargs="*", help="result folders (default: every harness/results/*-r4)")
    args = parser.parse_args()
    if args.command == "serve":
        serve(args.port, args.out)
    elif args.command == "push":
        check_push_args(parser, args)
        push(args)
    else:
        report(args.dirs)


if __name__ == "__main__":
    main()
