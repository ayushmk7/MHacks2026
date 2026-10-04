"""T-CON1 and T-CON2 (WP34, contacts). Needs two badges with a dev build.

Both badges must be on the same ESP-NOW channel: joined to the same hotspot, or neither joined to
any network. The test reads both channels and says so if they differ. No network is used.

Both badges run a helper app written and pushed by this test (contest). In mode "swap" it does
what the Contacts app does: it broadcasts wallet.contact_hello() once a second, answers a HELLO
from another badge with wallet.contact_card(), and hands a CARD it receives to
wallet.contact_accept().

  T-CON1  Two badges swap contacts: each accepts the other's card and then lists the other's name
          and address in wallet.contacts(). The frames are checked on the laptop too: the HELLO
          carries the badge's key and name, the CARD names the receiver's key and nonce, and its
          signature (domain "contact") verifies against the sender's key. The saved contact is
          still there after a reset.
  T-CON2  The CARD badge 1 accepted is captured from its log and handed to wallet.contact_accept
          again after the swap: "expired" (the nonce was rotated). Shown to badge 2, which it was
          not made for: "mismatch".

At the end each badge removes the other from its contacts (wallet.contact_remove) and gets its
display_name back.

One badge can check most of the firmware path alone, by swapping with itself (a badge accepts a
card it made for its own HELLO): single(badge) below. It is not run by this file's run(); a
two-line test file calls it:

    from t_con import single
    def run(badge): single(badge)
"""

import os
import re
import tempfile
import time

from common import b58enc, launch, to_launcher
from fixtures import badge_pubkey, case_lua, set_time, stop_app, verify

NEEDS = "two-badges"

APP = "contest"
NAME_A = "Con Badge A"
NAME_B = "Con Badge B"
SWAP_TIMEOUT_S = 30          # HELLO once a second, one signature and one verification on each side
DONE_TIMEOUT_S = 20

CONTACT_PREFIX = b"contact:"
T_HELLO = 16
T_CARD = 17

_CN = r"\[app\] CN "
_LUA_ERROR = r"\[lua\] .*(?:error|stopping)"

_APP_INI = (
    "name=Contact test\nversion=1.0.0\nauthor=BadgeOS tests\n"
    "description=t_con.py helper: swaps contact cards and lists the contacts.\n"
    "entry=main.lua\npermissions=contacts,espnow\nmin_api=2\n"
)

# The case (case.lua): mode is "swap", "replay", "self" or "list".
#   swap    broadcast HELLO once a second; answer a HELLO with a CARD; accept a CARD
#   replay  card_hex is handed to contact_accept (after a contact_hello, so a nonce is current)
#   self    one badge swaps with itself: hello, card, accept, accept again, hello, card, accept
#   list    remove (an address, optional) is removed first; then the contacts are listed
#
# Log lines, each "[app] CN ...":
#   up ch <channel>
#   hello <hex> | hello err <reason>          each time the HELLO frame changes
#   card to <mac> <true|false>                a CARD was made and sent (send result)
#   card ok | card err <reason>               mode self
#   got <i>/<n> <hex>                         a CARD arrived (a log line holds about 230 characters)
#   accept ok <address> <name> | accept err <reason>
#   remove <true|false>
#   count <n>, then n times: contact <i> <address> <added> <name>, then: listed
#   done                                      the modes other than swap
_APP_LUA = r"""
local case = require("case")
local gfx, wallet, espnow, codec = badge.gfx, badge.wallet, badge.espnow, badge.codec

local T_HELLO, T_CARD = 16, 17
local RESEND_MS = 3000       -- a card is sent again if its receiver still shows the same HELLO

local status = "starting"
local step = 0
local last_hello = nil
local last_hello_ms = nil
local answered = {}          -- mac -> { hello = the HELLO answered, at = when }
local own_hello = nil
local own_card = nil

local function log(line)
  badge.log("CN " .. line)
  status = line
end

local function log_bytes(tag, bytes)
  local text = codec.hex(bytes)
  local parts = (#text + 127) // 128
  for i = 1, parts do
    log(string.format("%s %d/%d %s", tag, i, parts, text:sub((i - 1) * 128 + 1, i * 128)))
  end
end

local function frame_type(data)
  if #data >= 4 and data:sub(1, 2) == "VK" and data:byte(3) == 1 then return data:byte(4) end
  return nil
end

local function hello()
  local frame, reason = wallet.contact_hello()
  if not frame then
    log("hello err " .. tostring(reason))
    return nil
  end
  if frame ~= last_hello then
    last_hello = frame
    log("hello " .. codec.hex(frame))
  end
  return frame
end

local function accept(card)
  local saved, reason = wallet.contact_accept(card)
  if saved then
    log("accept ok " .. saved.address .. " " .. saved.name)
  else
    log("accept err " .. tostring(reason))
  end
end

local function list()
  local all = wallet.contacts()
  log("count " .. #all)
  for i, c in ipairs(all) do
    log(string.format("contact %d %s %s %s", i, c.address, tostring(c.added), c.name))
  end
  log("listed")
end

-- One step per frame: each step is at most one signature or one verification.
local SCRIPTS = {
  replay = {
    function() hello() end,
    function() accept(codec.unhex(case.card_hex)) end,
    list,
  },
  self = {
    function()
      own_hello = hello()
      local card, reason = wallet.contact_card(own_hello)
      own_card = card
      log(card and "card ok" or ("card err " .. tostring(reason)))
    end,
    function() accept(own_card) end,
    function() accept(own_card) end,          -- the same card again: the nonce was rotated
    function()
      own_hello = hello()                     -- a new nonce
      own_card = wallet.contact_card(own_hello)
    end,
    function() accept(own_card) end,
    list,
  },
  list = {
    function()
      if case.remove then log("remove " .. tostring(wallet.contact_remove(case.remove))) end
    end,
    list,
  },
}

function on_start()
  espnow.enable(true)
  log("up ch " .. espnow.channel())
end

function on_update(dt)
  if case.mode == "swap" then
    local now = badge.millis()
    if not last_hello_ms or now - last_hello_ms >= 1000 then
      last_hello_ms = now
      local frame = hello()
      if frame then espnow.broadcast(frame) end
    end
    return
  end
  local script = SCRIPTS[case.mode] or {}
  if step > #script then return end
  step = step + 1
  if step > #script then
    log("done")
  else
    script[step]()
  end
end

function on_espnow(mac, data, rssi)
  if case.mode ~= "swap" then return end
  local kind = frame_type(data)
  if kind == T_HELLO then
    -- Answer each HELLO of a badge once. The same HELLO still showing after a while means the
    -- card was lost: send another. A different HELLO means the badge accepted the card (its nonce
    -- was rotated), and answering that one too would swap for ever.
    local now = badge.millis()
    local before = answered[mac]
    if before and (before.hello ~= data or now - before.at < RESEND_MS) then return end
    local card, reason = wallet.contact_card(data)
    if card then
      answered[mac] = { hello = data, at = now }
      log("card to " .. mac .. " " .. tostring(espnow.send(mac, card)))
    else
      log("card err " .. tostring(reason))
    end
  elseif kind == T_CARD then
    log_bytes("got", data)
    accept(data)
    list()
  end
end

function on_draw()
  gfx.clear(gfx.BG)
  gfx.text("contest", 12, 10, gfx.WHITE, 2)
  gfx.text(tostring(case.mode), 12, 40, gfx.GREEN, 1)
  gfx.text(status:sub(1, 48), 12, 56, gfx.WHITE, 1)
end

function on_button(key, pressed)
  if pressed and key == "b" then badge.system.exit() end
end
"""


# ---- helpers ------------------------------------------------------------------------------------

def start(badge, case):
    """Pushes contest with this case, clears the log and starts it."""
    stop_app(badge, APP)
    with tempfile.TemporaryDirectory() as folder:
        for name, text in (("app.ini", _APP_INI), ("main.lua", _APP_LUA), ("case.lua", case_lua(case))):
            with open(os.path.join(folder, name), "w", encoding="utf-8") as handle:
                handle.write(text)
        badge.push(folder, APP)
    badge.clear_log()
    launch(badge, APP)


def cn_wait(badge, pattern, timeout=10):
    """The first 'CN <pattern>' log line since the last clear_log(), as the text after 'CN '. Fails
    at once if the Lua runtime reports an error instead."""
    line = badge.wait_log("%s(?:%s)|%s" % (_CN, pattern, _LUA_ERROR), timeout=timeout)
    found = re.search(_CN, line)
    assert found, "contest failed instead of logging 'CN %s': %s" % (pattern, line)
    return line[found.end():].strip()


def cn_lines(badge, pattern):
    """Every 'CN <pattern>' line since the last clear_log(), as the text after 'CN '."""
    out = []
    for line in badge.log():
        found = re.search(_CN + "(" + pattern + ")", line)
        if found:
            out.append(found.group(1).strip())
    return out


def run_script(badge, case):
    """Runs one of the scripted modes to its end ('CN done')."""
    start(badge, case)
    cn_wait(badge, "done", timeout=DONE_TIMEOUT_S)


def set_name(badge, name):
    """Sets display_name (not a secure key) and returns the value it had."""
    before = badge.ok("VKGET display_name")
    reply = badge.cmd("VKSET display_name %s" % name)[-1]
    assert reply == "OK", "VKSET display_name %r -> %s" % (name, reply)
    return before


def listed(badge):
    """The contacts of the last complete listing in the log, as dicts {address, added, name}."""
    current, complete = None, None
    for line in cn_lines(badge, r"(?:count \d+|contact \d+ .*|listed)"):
        if line.startswith("count "):
            current = {"count": int(line.split()[1]), "rows": []}
        elif line.startswith("contact ") and current is not None:
            parts = line.split(" ", 4)
            assert len(parts) == 5, "malformed line: CN %s" % line
            current["rows"].append({"address": parts[2], "added": int(float(parts[3])), "name": parts[4]})
        elif line == "listed" and current is not None:
            assert len(current["rows"]) == current["count"], "the listing is incomplete: %s" % current
            complete, current = current["rows"], None
    assert complete is not None, "no complete contact listing in the log"
    return complete


def find_contact(rows, key):
    """The listed contact with that key, or None."""
    address = b58enc(key)
    matches = [row for row in rows if row["address"] == address]
    assert len(matches) <= 1, "the key %s is listed %d times" % (address, len(matches))
    return matches[0] if matches else None


def hello_frames(badge):
    """The distinct HELLO frames the app logged since the last clear_log(), in order."""
    return [bytes.fromhex(line.split()[1]) for line in cn_lines(badge, r"hello [0-9a-f]+")]


def got_cards(badge):
    """The CARD frames the app logged on arrival ('CN got i/n hex'), put back together, in order."""
    cards, parts = [], []
    for line in cn_lines(badge, r"got \d+/\d+ [0-9a-f]+"):
        _, position, text = line.split()
        index, total = (int(n) for n in position.split("/"))
        if index == 1:
            parts = []
        parts.append(text)
        if index == total and len(parts) == total:
            cards.append(bytes.fromhex("".join(parts)))
    return cards


def parse_hello(frame):
    """A CONTACT_HELLO frame as {pubkey, nonce, name} (protocol/espnow.md)."""
    assert frame[:4] == b"VK\x01" + bytes([T_HELLO]), "not a CONTACT_HELLO frame: %s" % frame.hex()
    name_len = frame[52]
    assert 1 <= name_len <= 32 and len(frame) == 53 + name_len, "HELLO length: %d bytes, name_len %d" % (
        len(frame), name_len)
    return {"pubkey": frame[4:36], "nonce": frame[36:52], "name": frame[53:].decode("ascii")}


def parse_card(frame):
    """A CONTACT_CARD frame as {peer_pubkey, peer_nonce, pubkey, name, sig, signed}; `signed` is
    the message its signature covers ("contact:" and the card body)."""
    assert frame[:4] == b"VK\x01" + bytes([T_CARD]), "not a CONTACT_CARD frame: %s" % frame.hex()
    name_len = frame[84]
    assert 1 <= name_len <= 32 and len(frame) == 85 + name_len + 64, "CARD length: %d bytes, name_len %d" % (
        len(frame), name_len)
    name = frame[85:85 + name_len]
    card = {"peer_pubkey": frame[4:36], "peer_nonce": frame[36:52], "pubkey": frame[52:84],
            "name": name.decode("ascii"), "sig": frame[85 + name_len:]}
    card["signed"] = CONTACT_PREFIX + card["peer_nonce"] + card["peer_pubkey"] + card["pubkey"] + bytes([name_len]) + name
    return card


def contact_sign_sizes(badge):
    """The sizes of the '[vk] sign contact <n> bytes <ms> ms' lines since the last clear_log()."""
    pattern = re.compile(r"\[vk\] sign contact (\d+) bytes \d+ ms")
    return [int(m.group(1)) for m in (pattern.search(line) for line in badge.log()) if m]


def check_card(card_frame, hello_frame, owner_key, owner_name, receiver_key):
    """A CARD against the HELLO it answers: made for that badge and that nonce, by that owner."""
    card, hello = parse_card(card_frame), parse_hello(hello_frame)
    assert card["peer_pubkey"] == receiver_key, "the card is made for %s" % b58enc(card["peer_pubkey"])
    assert card["peer_nonce"] == hello["nonce"], "the card does not carry the receiver's nonce"
    assert card["pubkey"] == owner_key, "the card's owner is %s" % b58enc(card["pubkey"])
    assert card["name"] == owner_name, "the card's name is %r" % card["name"]
    assert verify(owner_key, card["signed"], card["sig"]), "the card's signature does not verify (domain contact)"


# ---- the two-badge test -------------------------------------------------------------------------

def swap(a, b):
    """Starts the swap on both badges and waits for each to accept (or refuse) a card. Returns the
    two 'accept ...' lines."""
    start(a, {"mode": "swap"})
    start(b, {"mode": "swap"})
    channels = [int(cn_wait(each, r"up ch \d+", timeout=5).split()[2]) for each in (a, b)]
    assert channels[0] == channels[1], (
        "the badges are on different ESP-NOW channels (%d and %d): join both to the same hotspot, "
        "or neither to any" % tuple(channels))
    return [cn_wait(each, r"accept (?:ok|err) .*", timeout=SWAP_TIMEOUT_S) for each in (a, b)]


def forget(badge, other_key, old_name, strict):
    """Removes `other_key` from the badge's contacts and gives the badge its name back. With
    `strict` false (the test has already failed) nothing here raises."""
    try:
        try:
            run_script(badge, {"mode": "list", "remove": b58enc(other_key)})
            assert find_contact(listed(badge), other_key) is None, "contact_remove left the contact in the list"
            stop_app(badge, APP)
        finally:
            set_name(badge, old_name)
            to_launcher(badge)
    except Exception:
        if strict:
            raise


def run(badge, badge2):
    a, b = badge, badge2
    old_names = {}
    for each, name in ((a, NAME_A), (b, NAME_B)):
        to_launcher(each)
        old_names[each] = set_name(each, name)
        set_time(each)
    key_a, key_b = badge_pubkey(a), badge_pubkey(b)
    assert key_a != key_b, "both badges report the same key"

    passed = False
    try:
        # ---- T-CON1: two badges swap contacts ------------------------------------------------
        started = int(time.time())
        got_a, got_b = swap(a, b)
        if "accept err expired" in (got_a, got_b):
            # A nonce drawn by an earlier run can reach its 60 s between the HELLO and the CARD.
            # The nonces are fresh now, so a second swap cannot meet that.
            print("note: a swap nonce ran out during the swap; swapping once more")
            got_a, got_b = swap(a, b)
        assert got_a == "accept ok %s %s" % (b58enc(key_b), NAME_B), "badge 1: %s" % got_a
        assert got_b == "accept ok %s %s" % (b58enc(key_a), NAME_A), "badge 2: %s" % got_b
        for each in (a, b):
            cn_wait(each, "listed", timeout=5)

        for each, other_key, other_name, which in ((a, key_b, NAME_B, 1), (b, key_a, NAME_A, 2)):
            row = find_contact(listed(each), other_key)
            assert row is not None, "badge %d does not list the other badge: %s" % (which, listed(each))
            assert row["name"] == other_name, "badge %d lists the name %r" % (which, row["name"])
            assert started - 5 <= row["added"] <= int(time.time()) + 5, "badge %d: added %d" % (which, row["added"])
            assert each.state().get("notes", 1) >= 1, "badge %d has no 'Contact saved' note" % which

        # The frames, checked on the laptop: each badge's HELLO, and the card it accepted.
        card_for_a, card_for_b = got_cards(a)[0], got_cards(b)[0]
        for each, card, own_key, own_name, other_key, other_name, which in (
                (a, card_for_a, key_a, NAME_A, key_b, NAME_B, 1), (b, card_for_b, key_b, NAME_B, key_a, NAME_A, 2)):
            nonce = parse_card(card)["peer_nonce"]
            answered = [frame for frame in hello_frames(each) if parse_hello(frame)["nonce"] == nonce]
            assert answered, "badge %d accepted a card for a nonce it never announced" % which
            hello = parse_hello(answered[0])
            assert hello["pubkey"] == own_key and hello["name"] == own_name, "badge %d's HELLO: %s" % (which, hello)
            check_card(card, answered[0], other_key, other_name, own_key)
            # What the badge signed for its own card: "contact:" (8) + nonce 16 + two keys 64 +
            # name_len 1 + its name.
            sizes = contact_sign_sizes(each)
            assert 8 + 81 + len(own_name) in sizes, "sizes signed on badge %d: %s" % (which, sizes)

        # After the accept each badge announces a new nonce.
        time.sleep(2.5)
        for each, card, which in ((a, card_for_a, 1), (b, card_for_b, 2)):
            newest = parse_hello(hello_frames(each)[-1])
            assert newest["nonce"] != parse_card(card)["peer_nonce"], (
                "badge %d still announces the nonce it accepted a card for" % which)
        print("T-CON1: badge 1 saved %r, badge 2 saved %r" % (NAME_B, NAME_A))
        stop_app(a, APP)
        stop_app(b, APP)

        # ---- T-CON2: the captured CARD, replayed after the swap ------------------------------
        run_script(a, {"mode": "replay", "card_hex": card_for_a})
        replayed = cn_lines(a, r"accept (?:ok|err) .*")
        assert replayed == ["accept err expired"], "a replayed card on the badge it was made for: %s" % replayed
        assert find_contact(listed(a), key_b)["name"] == NAME_B, "the replay changed badge 1's contacts"
        run_script(b, {"mode": "replay", "card_hex": card_for_a})
        elsewhere = cn_lines(b, r"accept (?:ok|err) .*")
        assert elsewhere == ["accept err mismatch"], "a card shown to a badge it was not made for: %s" % elsewhere
        assert find_contact(listed(b), key_b) is None, "badge 2 saved a card that was made for badge 1"
        print("T-CON2: replayed on its receiver: expired; shown to another badge: mismatch")
        stop_app(b, APP)

        # ---- the contact survives a reset ----------------------------------------------------
        stop_app(a, APP)
        a.reset()
        run_script(a, {"mode": "list"})
        row = find_contact(listed(a), key_b)
        assert row is not None and row["name"] == NAME_B, "after a reset badge 1 lists: %s" % listed(a)
        passed = True
    finally:
        # Each badge forgets the other, and gets its name back.
        try:
            forget(a, key_b, old_names[a], passed)
        finally:
            forget(b, key_a, old_names[b], passed)


# ---- one badge, swapping with itself ------------------------------------------------------------

def single(badge):
    """What one badge can check alone: it makes a card for its own HELLO and accepts it.

    hello, card, accept (saved under its own key and name), the same card again (expired: the
    nonce was rotated), a new HELLO with a new nonce, a second swap; then the name is changed and
    the swap repeated (the stored name is updated, `added` is not), the contact survives a reset,
    and contact_remove removes it. Leaves the badge's contacts and display_name as they were."""
    name_1, name_2 = "Con Self", "Con Self 2"
    to_launcher(badge)
    old_name = set_name(badge, name_1)
    key = badge_pubkey(badge)
    address = b58enc(key)
    passed = False
    try:
        started = set_time(badge)
        run_script(badge, {"mode": "list", "remove": address})   # a clean start, whatever a run left behind
        before = len(listed(badge))

        run_script(badge, {"mode": "self"})
        assert cn_lines(badge, r"card (?:ok|err \S+)") == ["card ok"], "contact_card: %s" % cn_lines(badge, "card .*")
        accepts = cn_lines(badge, r"accept (?:ok|err) .*")
        ok = "accept ok %s %s" % (address, name_1)
        assert accepts == [ok, "accept err expired", ok], "the three accepts: %s" % accepts

        hellos = hello_frames(badge)
        assert len(hellos) == 2, "HELLO frames logged: %d" % len(hellos)
        first, second = parse_hello(hellos[0]), parse_hello(hellos[1])
        assert first["pubkey"] == key and first["name"] == name_1, "the HELLO: %s" % first
        assert second["nonce"] != first["nonce"], "the nonce was not rotated after the accept"
        sizes = contact_sign_sizes(badge)
        assert sizes == [8 + 81 + len(name_1)] * 2, "contact signatures (bytes): %s" % sizes

        rows = listed(badge)
        row = find_contact(rows, key)
        assert len(rows) == before + 1 and row is not None, "after the swap the badge lists: %s" % rows
        assert row["name"] == name_1, "the saved name: %r" % row["name"]
        assert started - 5 <= row["added"] <= int(time.time()) + 5, "added: %d" % row["added"]
        added = row["added"]
        assert badge.state().get("notes", 1) >= 1, "the badge has no 'Contact saved' note"

        # Another name: the same contact, renamed (the file is written over an existing one).
        stop_app(badge, APP)
        set_name(badge, name_2)
        time.sleep(1.2)                     # so a rewritten `added` would differ
        run_script(badge, {"mode": "self"})
        rows = listed(badge)
        row = find_contact(rows, key)
        assert len(rows) == before + 1 and row is not None and row["name"] == name_2, "after the rename: %s" % rows
        assert row["added"] == added, "updating the name changed `added`: %d, was %d" % (row["added"], added)

        # It is in the file, not only in memory.
        stop_app(badge, APP)
        badge.reset()
        run_script(badge, {"mode": "list"})
        row = find_contact(listed(badge), key)
        assert row is not None and row["name"] == name_2 and row["added"] == added, (
            "after a reset the badge lists: %s" % listed(badge))
        print("contacts on one badge: saved %r, renamed to %r, kept over a reset" % (name_1, name_2))
        passed = True
    finally:
        forget(badge, key, old_name, passed)
