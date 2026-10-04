"""T-APR1 in full, with the signtest app, the dashboard's badge listener and devnet (testing.md,
Acceptance tests; apps/apps.md, "Sign test"; integration/backend.md, "Existing routes"). Gate 1.

NEEDS a network, and this setup, which the test checks but does not make:

  - The badge is provisioned for the real deployment, not with the test values:
        scripts/vkdev.py --port P provision --env ../dashboard/.env --cap 100.00 --max 1000.00 \\
            --listener http://<laptop address on the hotspot>:8788 --wifi <ssid> <password>
    and has joined the hotspot (VKINFO wifi=1).
  - dashboard/.env has HACK_MINT (npm run devnet:setup), ATTACK_TX_VERSION=legacy and
    BADGE_LISTEN_HOST set to the laptop's hotspot address; `npm run server` is running.
  - The badge's key is in dashboard/server/config/badges.json and is funded (devnet SOL for the
    fee, a token account, HACK).
  - The laptop reaches the dashboard's admin API (default http://127.0.0.1:8787; set
    VK_DASHBOARD_API to change it) and the RPC node the badge is provisioned with.

In order:
  1. The app polls the listener and shows "waiting".
  2. The test asks the dashboard for a transfer out of this badge (POST /api/attacks) for
     AMOUNT. The app fetches it and opens the approval: red "UNVERIFIED RECIPIENT" with the dev
     override, showing the amount decoded from the bytes.
  3. An injected hold signs. The signature the app logs verifies on the laptop over the served
     message with the badge's VKINFO key; the app submits it; the RPC node reports the
     transaction confirmed.
  4. A second transfer is refused with CANCEL: the app reports it, and the dashboard marks the
     attempt rejected.

The test does not provision or reset the badge. It leaves signtest installed and the launcher on
the screen. Each run moves AMOUNT of the badge's test tokens to the dashboard's attacker account.
"""

import base64
import json
import os
import re
import time
import urllib.error
import urllib.request

from common import HOLD_MARGIN_MS, b58dec, hold_ms, launch, to_launcher
from fixtures import badge_pubkey, verify

NEEDS = "network"

_HERE = os.path.dirname(os.path.abspath(__file__))
SIGNTEST_DIR = os.path.normpath(os.path.join(_HERE, "..", "..", "apps", "signtest"))
SIGNTEST = "signtest"

API = os.environ.get("VK_DASHBOARD_API", "http://127.0.0.1:8787").rstrip("/")
AMOUNT = 1.0                 # display units; small, and under any sensible cap
CONFIRM_TIMEOUT_S = 90


def http_json(url, body=None, timeout=20):
    """GET, or POST of a JSON body. Returns (status, parsed JSON or None)."""
    data = None if body is None else json.dumps(body).encode("utf-8")
    request = urllib.request.Request(url, data=data, headers={"Content-Type": "application/json"})
    try:
        with urllib.request.urlopen(request, timeout=timeout) as reply:
            status, text = reply.status, reply.read().decode("utf-8")
    except urllib.error.HTTPError as exc:
        status, text = exc.code, exc.read().decode("utf-8", "replace")
    try:
        return status, json.loads(text) if text else None
    except ValueError:
        return status, None


def format_amount(raw, decimals):
    """Raw units as the firmware prints them: 1250, 2 -> "12.50"."""
    digits = str(int(raw)).rjust(decimals + 1, "0")
    return digits if decimals == 0 else digits[:-decimals] + "." + digits[-decimals:]


def new_attempt(address):
    """Asks the dashboard to build an unsigned transfer out of this badge."""
    try:
        status, attempt = http_json(API + "/api/attacks", {"victim": address, "displayAmount": AMOUNT, "actualAmount": AMOUNT})
    except OSError as exc:
        raise AssertionError("the dashboard's admin API at %s does not answer (%s): is `npm run server` running?" % (API, exc))
    assert status == 201 and attempt and attempt.get("id"), "POST /api/attacks -> %s %s" % (status, attempt)
    assert attempt.get("txVersion") == "legacy", (
        "the dashboard built a %r message; set ATTACK_TX_VERSION=legacy (the badge refuses versioned messages)"
        % attempt.get("txVersion"))
    for warning in attempt.get("warnings") or []:
        print("dashboard warning: %s" % warning)
    return attempt


def attempt_outcome(attempt_id):
    status, listing = http_json(API + "/api/attacks?limit=50")
    assert status == 200 and listing, "GET /api/attacks -> %s" % status
    for attempt in listing["attempts"]:
        if attempt["id"] == attempt_id:
            return attempt["outcome"]
    return None


def confirmed(rpc_url, signature):
    """True once the RPC node reports the transaction confirmed. Raises if it failed on chain."""
    body = {"jsonrpc": "2.0", "id": 1, "method": "getSignatureStatuses",
            "params": [[signature], {"searchTransactionHistory": True}]}
    status, reply = http_json(rpc_url, body)
    assert status == 200 and reply and "result" in reply, "getSignatureStatuses -> %s %s" % (status, reply)
    entry = reply["result"]["value"][0]
    if entry is None:
        return False
    assert entry.get("err") is None, "the transaction failed on chain: %s" % json.dumps(entry["err"])
    return entry.get("confirmationStatus") in ("confirmed", "finalized")


def wait_approval(badge, attempt):
    """Waits for the app to open the approval for this attempt and checks what the firmware shows."""
    badge.wait_log(r"\[app\] ST begin %s" % re.escape(attempt["id"]), timeout=20)
    state = badge.wait_state(lambda s: s["modal"] and s["phase"] == "ARMED", timeout=5)
    big = "%s %s" % (format_amount(attempt["decoded"]["amountRaw"], attempt["decimals"]), attempt["symbol"])
    assert state["title"] == "Pay" and state["app"] == SIGNTEST, "approval: %s" % state
    assert state["severity"] == "red" and state["headline"] == "UNVERIFIED RECIPIENT", "approval: %s %r" % (
        state["severity"], state["headline"])
    assert state["big"] == big, "the approval shows %r; the served bytes say %r" % (state["big"], big)
    assert state["dev_override"] is True and state["select"] == "hold", (
        "no dev override: select %s, dev_override %r" % (state["select"], state["dev_override"]))
    return state


def run(badge):
    info = badge.info()
    assert info.get("profile") == "dev", "T-APR1 signs through the dev override; this build's profile is %s" % info.get("profile")
    assert info.get("provisioned") == "1", "the badge is not provisioned (see this file's header)"
    assert info.get("wifi") == "1", "the badge is not on Wi-Fi (VKINFO wifi=%s)" % info.get("wifi")
    listener = badge.ok("VKGET listener_url")
    rpc_url = badge.ok("VKGET rpc_url")
    assert listener, "listener_url is not set on the badge"
    assert rpc_url, "rpc_url is not set on the badge"
    address = info["pubkey"]
    own = badge_pubkey(badge)
    hold = hold_ms(badge) + HOLD_MARGIN_MS

    to_launcher(badge)
    badge.push(SIGNTEST_DIR, SIGNTEST)
    badge.clear_log()
    launch(badge, SIGNTEST)
    try:
        # 1. The listener answers the app.
        badge.wait_log(r"\[app\] ST status waiting", timeout=20)

        # 2. A transfer is served and shown.
        attempt = new_attempt(address)
        msg = base64.b64decode(attempt["messageBase64"])
        wait_approval(badge, attempt)

        # 3. Signed after the hold, verified here, confirmed on chain.
        badge.btn("a", "hold", hold)
        line = badge.wait_log(r"\[app\] ST (?:sig|refused) \S+", timeout=15)
        assert " ST sig " in line, "the payment was not signed: %s" % line
        signature = line.split()[-1]
        assert verify(own, msg, b58dec(signature)), "the signature does not verify over the served message with the badge's key"
        print("M2 %s" % badge.wait_log(r"\[vk\] sign solana \d+ bytes \d+ ms", timeout=3).strip())

        line = badge.wait_log(r"\[app\] ST (?:sent \S+|status not sent.*)", timeout=60)
        assert " ST sent " in line, "the app could not submit the transaction: %s" % line
        assert line.split()[-1] == signature, "the RPC node answered %s for a transaction signed %s" % (line.split()[-1], signature)

        deadline = time.monotonic() + CONFIRM_TIMEOUT_S
        while not confirmed(rpc_url, signature):
            assert time.monotonic() < deadline, "%s was not confirmed within %d s" % (signature, CONFIRM_TIMEOUT_S)
            time.sleep(2)
        print("confirmed on chain: %s" % signature)
        print("dashboard outcome of the attempt: %s (it becomes 'signed' when the ingest sees the transfer)"
              % attempt_outcome(attempt["id"]))

        # 4. A refusal is reported to the laptop.
        badge.clear_log()
        attempt = new_attempt(address)
        wait_approval(badge, attempt)
        badge.btn("b", "tap")
        badge.wait_state(lambda s: not s["modal"], timeout=5)
        badge.wait_log(r"\[app\] ST refused unverified", timeout=10)
        deadline = time.monotonic() + 30
        while attempt_outcome(attempt["id"]) != "rejected":
            assert time.monotonic() < deadline, "the dashboard did not record the refusal (POST /badge/outcome)"
            time.sleep(1)
    finally:
        to_launcher(badge)
