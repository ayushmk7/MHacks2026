"""Helpers shared by the scripted device tests (execution-plan.md, section 5.2).

A test is test/device/t_<name>.py with `def run(badge):`; `badge` is the Badge of scripts/vkdev.py.
vkdev.py puts this folder on the import path, so a test writes `from common import ...`.
"""

import json
import os
import sys
import time

_HERE = os.path.dirname(os.path.abspath(__file__))
_SCRIPTS = os.path.normpath(os.path.join(_HERE, "..", "..", "scripts"))
if _SCRIPTS not in sys.path:
    sys.path.insert(0, _SCRIPTS)

from vkdev import b58dec, b58enc  # noqa: E402,F401  (re-exported: one base58 for the tool and the tests)

VECTORS_PATH = os.path.normpath(os.path.join(_HERE, "..", "host", "vectors.json"))

# How long past the configured hold time an injected hold lasts.
HOLD_MARGIN_MS = 300


def load_vectors():
    """test/host/vectors.json as a dict (test keys, messages, a record, a request, a proof)."""
    with open(VECTORS_PATH, "r", encoding="utf-8") as handle:
        return json.load(handle)


def test_config(vectors=None):
    """The values a badge is provisioned with for device tests (execution-plan.md, section 1)."""
    vectors = vectors or load_vectors()
    mint = vectors["tx"].get("mint") or b58enc(bytes.fromhex(vectors["tx"]["mint_hex"]))
    return {
        "issuer_key": vectors["vk"]["issuer"]["pubkey_b58"],
        "tokens": "%s:2:HACK:100.00:1000.00" % mint,
        "rpc_url": "http://127.0.0.1:8899",
        "approval_tmo_s": "10",
        "hold_ms": "1000",
        "record_ttl_s": "3600",
    }


def hold_ms(badge):
    """The badge's hold-SELECT time. 1000 (the test value) if the key cannot be read."""
    try:
        return int(badge.ok("VKGET hold_ms"))
    except (AssertionError, ValueError):
        return 1000


def provision_test(badge):
    """Leaves the badge provisioned with test_config(). A provisioned badge is reset first (VKRESET
    and an injected hold on its confirmation), because its secure keys cannot be changed silently."""
    if badge.info().get("provisioned") == "1":
        hold = hold_ms(badge) + HOLD_MARGIN_MS
        badge.ok("VKRESET")
        badge.wait_state(lambda s: s["modal"] and s["phase"] == "ARMED", timeout=5)
        badge.btn("a", "hold", hold)
        badge.wait_state(lambda s: not s["modal"], timeout=5)
        assert badge.info().get("provisioned") == "0", "VKRESET did not erase the wallet config"

    values = test_config()
    listed = [line.split()[1] for line in badge.cmd("VKKEYS")[:-1] if len(line.split()) > 1]
    for key in listed:
        if key in values:
            reply = badge.cmd("VKSET %s %s" % (key, values[key]))[-1]
            assert reply == "OK", "VKSET %s -> %s" % (key, reply)
    badge.ok("VKCOMMIT")
    assert badge.info().get("provisioned") == "1", "VKCOMMIT answered OK but provisioned is not 1"
    return values


def to_launcher(badge):
    """Stops whatever is running and leaves the launcher on screen.

    Upstream answers a STOP by showing its launcher (cursor on the first row), or its error screen
    if an app error is stored. CANCEL dismisses that error screen, but on the launcher it opens
    Settings, and the state does not say which of the two is showing. So: STOP, CANCEL, STOP. The
    second STOP finds no stored error and always lands on the launcher."""
    if badge.state()["modal"]:
        badge.btn("b", "tap")
        badge.wait_state(lambda s: not s["modal"], timeout=5)
    badge.stop()
    time.sleep(0.3)
    if badge.state()["app"]:
        return  # a home app took the screen back: that is the launcher
    badge.btn("b", "tap")
    badge.stop()
    time.sleep(0.3)


def launch(badge, app_id, timeout=10):
    """Starts an app and returns the state once it is running. Approves the first-run consent
    screen (title "Allow app") with an injected hold if it appears."""
    badge.run(app_id)
    deadline = time.monotonic() + timeout
    consented = False
    while True:
        state = badge.state()
        if state["modal"] and state["title"] == "Allow app" and not consented:
            badge.wait_state(lambda s: s["phase"] == "ARMED" or not s["modal"], timeout=5)
            badge.btn("a", "hold", hold_ms(badge) + HOLD_MARGIN_MS)
            badge.wait_state(lambda s: not s["modal"], timeout=5)
            consented = True
        elif state["app"] == app_id and not state["modal"]:
            return state
        assert time.monotonic() < deadline, "%s did not start within %g s; state: %s" % (
            app_id, timeout, json.dumps(state))
        time.sleep(0.1)
