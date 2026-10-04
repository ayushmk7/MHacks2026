"""T-REQ1 to T-REQ4 and T-CHK1 (WP23, requests and presence). Needs two badges with a dev build.

`badge` is the payer (A), `badge2` the payee (B). Both must be on the same ESP-NOW channel: joined
to the same hotspot, or neither joined to any network. The test reads both channels and says so if
they differ. It provisions both badges with the test values and sets both clocks (VKTIME), so
presence can come out green. No network is used: the registry record for B's key is made on the
laptop with the test issuer key (fixtures.py), and so is the transfer A signs.

B runs the reqtest app (apps/reqtest): it opens a request for 10.00 and logs its req_id.
A runs a helper app written and pushed by this test (reqpay): it finds that request in
wallet.requests(), challenges, follows wallet.presence(), and calls wallet.begin_solana with the
record and the request it heard.

  T-REQ1  A lists B's request within 2 s, with the right amount, name claim and key.
  T-REQ2  A challenges; presence is "present" (the PROOF came back within presence_ms).
          The "[req] proof <ms> ms" line is printed: it is one sample of measurement M1.
  T-CHK1  valid record, valid request, presence present, clock synced: green VERIFIED - PRESENT,
          press. SELECT signs; the signature verifies on the laptop against A's key.
  T-REQ3  A replayed request. With two badges the replayer is played by a MAC that is not B's: A
          challenges that MAC (as it would challenge whoever it heard the replayed bytes from),
          nobody there holds B's key, so presence stays "pending" and the approval is amber
          VERIFIED - NOT PRESENT, hold. With a third badge, have it rebroadcast B's REQ bytes and
          pass its MAC as challenge_mac.
  T-REQ4  With req_max_proofs set to 3 on B, A sends nine CHALs for one request, 4 s apart (longer
          than a signature and req_gap_ms): B answers three and no more.

Not here: T-REQ5 (the notification) needs the inbox of WP32; T-REQ6 is in t_apr.py.
"""

import os
import re
import tempfile
import time

from common import b58enc, launch, provision_test, to_launcher
from fixtures import BEGIN_TIMEOUT_S, Keys, badge_pubkey, case_lua, set_time, verify
from t_req_single import sign_lines, start_reqtest, stop_reqtest

NEEDS = "two-badges"

PAYER = "reqpay"
PAYEE_NAME = "Test Shop"
REQUEST_TTL_S = 300          # long enough for the slowest section (nine challenges, 4 s apart)
REPLAYER_MAC = "02:00:00:00:be:ef"   # locally administered; no badge has it

_RP = r"\[app\] RP "
_LUA_ERROR = r"\[lua\] .*(?:error|stopping)"

_PAYER_INI = (
    "name=Request payer test\nversion=1.0.0\nauthor=Badge OS tests\n"
    "description=t_req.py helper: finds a request, challenges, begins a payment.\n"
    "entry=main.lua\npermissions=sign,espnow\nmin_api=2\n"
)

# The case (case.lua): req_id (16 hex) is the request to act on; msg_hex, record_hex, sig_hex are
# passed to begin_solana (no msg_hex: no payment); challenge_mac overrides the MAC to challenge;
# challenges is how many CHALs to send, challenge_gap_ms apart; wait_ms is how long to wait for
# presence to be decided after the last one.
#
# Log lines, each "[app] RP ...":
#   up ch <channel>
#   seen <req_id> <mac> <amount> <currency> <rail> <payee> <expires_in_s> <ms since start> <name>
#   chal <n> ok | chal <n> err <reason>
#   presence <state>                  on every change of wallet.presence(req_id)
#   begin ok | begin err <reason>
#   poll sig <hex> | poll err <reason>
#   done                              the case had no payment
_PAYER_LUA = r"""
local case = require("case")
local gfx, wallet = badge.gfx, badge.wallet

local started = badge.millis()
local stage = "find"          -- find -> challenge -> settle -> begin -> poll -> done
local entry = nil
local sent = 0
local last_chal = 0
local shown = nil
local status = "waiting for the request"

local function log(line)
  badge.log("RP " .. line)
  status = line
end

local function hex(bytes)
  local out = {}
  for i = 1, #bytes do out[i] = string.format("%02x", bytes:byte(i)) end
  return table.concat(out)
end

local function unhex(text)
  local out = {}
  for i = 1, #text, 2 do out[#out + 1] = string.char(tonumber(text:sub(i, i + 1), 16)) end
  return table.concat(out)
end

local function find()
  for _, r in ipairs(wallet.requests()) do
    if r.req_id == case.req_id then return r end
  end
end

local function watch_presence()
  local now = wallet.presence(case.req_id)
  if now ~= shown then
    shown = now
    log("presence " .. now)
  end
  return now
end

function on_start()
  badge.espnow.enable(true)
  log("up ch " .. badge.espnow.channel())
end

function on_update(dt)
  local now = badge.millis()
  if stage == "find" then
    entry = find()
    if entry then
      log(string.format("seen %s %s %s %s %s %s %d %d %s", entry.req_id, entry.mac, entry.amount,
                        entry.currency, entry.rail, entry.payee, entry.expires_in_s, now - started, entry.name))
      stage = "challenge"
    end
    return
  end

  local presence = watch_presence()

  if stage == "challenge" then
    if sent >= (case.challenges or 1) then
      stage = "settle"
    elseif sent == 0 or now - last_chal >= (case.challenge_gap_ms or 1500) then
      local ok, reason = wallet.challenge(case.challenge_mac or entry.mac, entry.req)
      sent = sent + 1
      last_chal = badge.millis()
      shown = nil               -- log the presence state again after every challenge
      log("chal " .. sent .. (ok and " ok" or (" err " .. tostring(reason))))
    end
  elseif stage == "settle" then
    if presence ~= "pending" or now - last_chal >= (case.wait_ms or 4000) then
      if case.msg_hex then
        stage = "begin"
      else
        stage = "done"
        log("done")
      end
    end
  elseif stage == "begin" then
    local ok, reason = wallet.begin_solana(unhex(case.msg_hex), {
      record = unhex(case.record_hex), record_sig = unhex(case.sig_hex), req = entry.req,
    })
    if ok then
      stage = "poll"
      log("begin ok")
    else
      stage = "done"
      log("begin err " .. tostring(reason))
    end
  elseif stage == "poll" then
    local result, reason = wallet.poll()
    if result == "pending" then return end
    stage = "done"
    if result then
      log("poll sig " .. hex(result))
    else
      log("poll err " .. tostring(reason))
    end
  end
end

function on_draw()
  gfx.clear(gfx.BG)
  gfx.text("reqpay", 12, 10, gfx.WHITE, 2)
  gfx.text(stage, 12, 40, gfx.SOLANA_GREEN, 1)
  gfx.text(status:sub(1, 48), 12, 56, gfx.WHITE, 1)
end

function on_button(key, pressed)
  if pressed and key == "b" then badge.system.exit() end
end
"""


# ---- helpers ------------------------------------------------------------------------------------

def rp_wait(badge, pattern, timeout=10):
    """The first 'RP <pattern>' log line since the last clear_log(), as the text after 'RP '. Fails
    at once if the Lua runtime reports an error instead."""
    line = badge.wait_log("%s(?:%s)|%s" % (_RP, pattern, _LUA_ERROR), timeout=timeout)
    found = re.search(_RP, line)
    assert found, "reqpay failed instead of logging 'RP %s': %s" % (pattern, line)
    return line[found.end():].strip()


def rp_lines(badge, pattern):
    """Every 'RP <pattern>' line since the last clear_log(), as the text after 'RP '."""
    out = []
    for line in badge.log():
        found = re.search(_RP + "(" + pattern + ")", line)
        if found:
            out.append(found.group(1).strip())
    return out


def stop_payer(badge):
    """Closes an open approval with CANCEL and stops reqpay if it is the running app."""
    state = badge.state()
    if state["modal"]:
        if state["phase"] != "RESULT":
            badge.btn("b", "tap")
        state = badge.wait_state(lambda s: not s["modal"], timeout=5)
    if state["app"] == PAYER:
        badge.stop()
        badge.wait_state(lambda s: s["app"] != PAYER, timeout=5)


def start_payer(badge, case):
    """Pushes reqpay with this case, clears the log and starts it."""
    stop_payer(badge)
    with tempfile.TemporaryDirectory() as folder:
        for name, text in (("app.ini", _PAYER_INI), ("main.lua", _PAYER_LUA), ("case.lua", case_lua(case))):
            with open(os.path.join(folder, name), "w", encoding="utf-8") as handle:
                handle.write(text)
        badge.push(folder, PAYER)
    badge.clear_log()
    launch(badge, PAYER)


def open_request(payee):
    """(Re)starts reqtest on the payee badge with one request for 10.00. Returns (req_id, channel)."""
    result = start_reqtest(payee, amount="10.00", count=1, name=PAYEE_NAME, ttl_s=REQUEST_TTL_S)
    assert result[0][0] == "open", "the payee could not open a request: %s" % (result,)
    line = payee.wait_log(r"\[app\] RT up ch (\d+)", timeout=5)
    return result[0][1], int(re.search(r"up ch (\d+)", line).group(1))


def payee_proofs(payee):
    """The proofs count of the payee's newest 'RT status' line (0 if none yet)."""
    counts = [int(m.group(1)) for m in (re.search(r"\[app\] RT status \S+ (\d+)", line) for line in payee.log()) if m]
    return counts[-1] if counts else 0


def proof_ms(badge):
    """The CHAL -> PROOF times the payer logged since the last clear_log()."""
    return [int(m.group(1)) for m in (re.search(r"\[req\] proof (\d+) ms", line) for line in badge.log()) if m]


def parse_seen(text):
    """The fields of an 'RP seen ...' line."""
    parts = text.split(" ", 9)
    assert len(parts) == 10 and parts[0] == "seen", "malformed line: RP %s" % text
    return {"req_id": parts[1], "mac": parts[2], "amount": parts[3], "currency": parts[4], "rail": parts[5],
            "payee": parts[6], "expires_in_s": int(parts[7]), "after_ms": int(parts[8]), "name": parts[9]}


# ---- the test -----------------------------------------------------------------------------------

def run(badge, badge2):
    payer, payee = badge, badge2
    for each in (payer, payee):
        to_launcher(each)
        provision_test(each)
        set_time(each)                      # both clocks synced: the best verdict is green
    presence_ms = int(payer.ok("VKGET presence_ms"))

    keys = Keys()
    payer_key = badge_pubkey(payer)
    payee_key = badge_pubkey(payee)
    assert payer_key != payee_key, "both ports are the same badge"

    # What the backend would serve for the payee, made here with the test issuer key: a record for
    # the payee badge's own device key. And the payment the payer signs: 10.00 to the record's account.
    now = int(time.time())
    record, record_sig = keys.record(issued_at=now - 5, expiry=now + 3600, device_pubkey=payee_key)
    message = keys.transfer(payer_key, 1000)
    payment = {"msg_hex": message, "record_hex": record, "sig_hex": record_sig}

    try:
        # ---- T-REQ1, T-REQ2, T-CHK1: the honest payment --------------------------------------
        req_id, payee_channel = open_request(payee)
        start_payer(payer, dict(payment, req_id=req_id))
        payer_channel = int(rp_wait(payer, r"up ch \d+").split()[-1])
        assert payer_channel == payee_channel, (
            "the badges are on different ESP-NOW channels (%d and %d): join both to the same hotspot, "
            "or neither to any network" % (payer_channel, payee_channel))

        seen = parse_seen(rp_wait(payer, "seen .*", timeout=10))
        print("T-REQ1: listed after %d ms: %s" % (seen["after_ms"], seen))
        assert seen["after_ms"] <= 2000, "the request was listed after %d ms, not within 2 s" % seen["after_ms"]
        assert seen["req_id"] == req_id
        assert seen["amount"] == "10.00" and seen["currency"] == keys.symbol, "amount: %s" % seen
        assert seen["rail"] == "solana", "rail: %s" % seen
        assert seen["name"] == PAYEE_NAME, "name claim: %s" % seen
        assert seen["payee"] == b58enc(payee_key), "payee key: %s, the payee badge is %s" % (
            seen["payee"], b58enc(payee_key))
        assert 0 < seen["expires_in_s"] <= REQUEST_TTL_S, "expires_in_s: %s" % seen

        assert rp_wait(payer, r"chal 1 \S+.*") == "chal 1 ok"
        decided = rp_wait(payer, r"presence (?:present|late|bad_sig)", timeout=10)
        times = proof_ms(payer)
        print("T-REQ2: %s; [req] proof ms: %s (presence_ms %d)" % (decided, times, presence_ms))
        assert decided == "presence present", "presence after the challenge: %s (proof ms %s)" % (decided, times)
        assert times and times[-1] <= presence_ms, "present, but the logged proof time is %s" % times

        assert rp_wait(payer, r"begin (?:ok|err \S+)", timeout=BEGIN_TIMEOUT_S) == "begin ok"
        state = payer.wait_state(lambda s: s["modal"] and s["phase"] == "ARMED", timeout=5)
        print("T-CHK1: %s / %s / %s" % (state["severity"], state["headline"], state["select"]))
        assert state["severity"] == "green", "severity: %s" % state
        assert state["headline"] == "VERIFIED - PRESENT", "headline: %s" % state
        assert state["select"] == "press", "select: %s" % state
        assert state["big"] == "10.00 %s" % keys.symbol, "big: %s" % state
        assert state["sub"] == "to %s" % keys.merchant_name, "sub: %s" % state

        payer.btn("a", "tap")
        payer.wait_state(lambda s: not s["modal"], timeout=10)
        polled = rp_wait(payer, r"poll (?:sig [0-9a-f]+|err \S+)", timeout=15)
        assert polled.startswith("poll sig "), "after SELECT the app polled: %s" % polled
        signature = bytes.fromhex(polled.split()[2])
        assert len(signature) == 64 and verify(payer_key, message, signature), (
            "the signature does not verify against the payer badge's key")

        time.sleep(1.5)                     # one more "RT status" line on the payee
        assert payee_proofs(payee) == 1, "the payee answered %d presence checks, not 1" % payee_proofs(payee)
        assert len(sign_lines(payee, "pay-proof")) == 1, "pay-proof signatures on the payee: %s" % (
            sign_lines(payee, "pay-proof"),)
        stop_payer(payer)

        # ---- T-REQ3: a replayed request ------------------------------------------------------
        req_id, _ = open_request(payee)     # a fresh request (stopping reqtest closed the last one)
        start_payer(payer, dict(payment, req_id=req_id, challenge_mac=REPLAYER_MAC, wait_ms=3000))
        rp_wait(payer, "seen .*", timeout=10)
        assert rp_wait(payer, r"chal 1 \S+.*") == "chal 1 ok"
        assert rp_wait(payer, r"begin (?:ok|err \S+)", timeout=BEGIN_TIMEOUT_S) == "begin ok"
        states = rp_lines(payer, r"presence \S+")   # "none" before the challenge, then "pending"
        assert "presence pending" in states and not set(states) - {"presence none", "presence pending"}, (
            "presence for a replayed request: %s" % states)
        state = payer.wait_state(lambda s: s["modal"] and s["phase"] == "ARMED", timeout=5)
        print("T-REQ3: %s / %s / %s" % (state["severity"], state["headline"], state["select"]))
        assert state["severity"] == "amber", "severity: %s" % state
        assert state["headline"] == "VERIFIED - NOT PRESENT", "headline: %s" % state
        assert state["select"] == "hold", "select: %s" % state
        payer.btn("b", "tap")
        payer.wait_state(lambda s: not s["modal"], timeout=5)
        assert rp_wait(payer, r"poll (?:sig [0-9a-f]+|err \S+)", timeout=15) == "poll err cancelled"
        assert payee_proofs(payee) == 0, "the payee answered a challenge that was sent to another MAC"
        stop_payer(payer)

        # ---- T-REQ4: the proof limit ---------------------------------------------------------
        assert payee.cmd("VKSET req_max_proofs 3")[-1] == "OK"
        req_id, _ = open_request(payee)
        start_payer(payer, {"req_id": req_id, "challenges": 9, "challenge_gap_ms": 4000, "wait_ms": 4000})
        rp_wait(payer, "seen .*", timeout=10)
        rp_wait(payer, r"chal 9 \S+.*", timeout=9 * 4 + 20)
        rp_wait(payer, "done", timeout=15)
        time.sleep(1.5)
        sent = rp_lines(payer, r"chal \d+ ok")
        answered = payee_proofs(payee)
        signed = len(sign_lines(payee, "pay-proof"))
        received = len(proof_ms(payer))
        print("T-REQ4: %d CHALs sent, payee answered %d (signed %d), payer judged %d" % (
            len(sent), answered, signed, received))
        assert len(sent) == 9, "challenges sent: %s" % sent
        assert answered == 3 and signed == 3, "with req_max_proofs 3 the payee answered %d (signed %d)" % (
            answered, signed)
        assert 1 <= received <= 3, "PROOFs judged by the payer: %d" % received
        stop_payer(payer)
    finally:
        payee.cmd("VKSET req_max_proofs 8")     # the default (platform/config.md)

    stop_reqtest(payee)
    to_launcher(payer)
    to_launcher(payee)
