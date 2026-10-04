"""Notifications and the Inbox app on one badge (WP32): VKNOTE posts, the count shows, the Inbox
opens the note's app, RIGHT dismisses.

1. A reset empties the inbox (notes live in RAM): VKSTATE notes is 0.
2. VKNOTE Test|hello|hello_native -> notes 1, and the launcher's picture changes (the status item
   "[1]"). The same title and body again within 10 s is ignored.
3. The Inbox app (native) lists it; SELECT launches hello_native and the note is gone.
4. A second note is dismissed with RIGHT: notes 0, the Inbox stays open and shows "Nothing new".
5. SELECT on a note whose app is not installed removes the note and launches nothing.
6. A ninth note pushes the oldest out: notes stays 8. RIGHT eight times empties the inbox.

Needs one badge with a dev build that has the native apps inbox and hello_native. No provisioning,
no network, no hands. T-REQ5 (a payment request seen over the air raises the note) needs a second
badge and is not here. The LED pattern `notify` cannot be seen over serial: look at the badge
while step 2's note waits (dim purple breathe, 3 s period).

The test resets the badge, so the clock is unset afterwards.
"""

import collections
import os
import struct

from common import launch, to_launcher

SHOTS = os.path.join(os.path.dirname(os.path.abspath(__file__)), "shots")
SHOT_BYTES = 320 * 240 * 2

INBOX = "inbox"
TARGET = "hello_native"
MAX_NOTES = 8


def _notes(badge):
    return badge.state()["notes"]


def _post(badge, title, body, app):
    reply = badge.cmd("VKNOTE %s|%s|%s" % (title, body, app))[-1]
    assert reply == "OK", "VKNOTE -> %s" % reply


def _not_blank(pixels, what):
    assert len(pixels) == SHOT_BYTES, "screenshot is %d bytes" % len(pixels)
    colours = collections.Counter(struct.unpack("<%dH" % (len(pixels) // 2), pixels))
    assert len(colours) > 1, "%s: the screen is blank" % what


def _open_inbox(badge):
    state = launch(badge, INBOX)
    assert state["app"] == INBOX and state["native"] is True, "the Inbox is not running: %s" % state
    return state


def _leave(badge, app):
    badge.btn("b", "tap")  # CANCEL
    badge.wait_state(lambda s: s["app"] != app, timeout=5)


def run(badge):
    os.makedirs(SHOTS, exist_ok=True)

    # 1. A known state: no notes, and no memory of an earlier run's posts (the 10 s rule).
    badge.reset()
    to_launcher(badge)
    assert _notes(badge) == 0, "notes is %d right after a reset" % _notes(badge)
    empty_launcher = badge.shot()

    # 2. One note. The count is in the state and on the launcher.
    _post(badge, "Test", "hello", TARGET)
    assert _notes(badge) == 1, "after VKNOTE, notes is %d" % _notes(badge)
    noted_launcher = badge.shot(os.path.join(SHOTS, "notify_launcher_1.png"))
    assert noted_launcher != empty_launcher, "the launcher looks the same with a note waiting (no [1])"

    _post(badge, "Test", "hello", TARGET)
    assert _notes(badge) == 1, "an identical note within 10 s was not ignored: notes is %d" % _notes(badge)

    # 3. The Inbox lists it; SELECT opens the note's app and removes the note.
    _open_inbox(badge)
    assert _notes(badge) == 1, "opening the Inbox changed the count to %d" % _notes(badge)
    listed = badge.shot(os.path.join(SHOTS, "notify_inbox.png"))
    _not_blank(listed, "Inbox")
    badge.btn("a", "tap")  # SELECT
    state = badge.wait_state(lambda s: s["app"] == TARGET, timeout=5)
    assert state["native"] is True, "VKSTATE native is %r while %s runs" % (state["native"], TARGET)
    assert state["notes"] == 0, "SELECT launched %s but notes is %d" % (TARGET, state["notes"])
    _leave(badge, TARGET)
    to_launcher(badge)

    # 4. RIGHT dismisses; the Inbox stays open and shows its empty text.
    _post(badge, "Second", "dismiss me", TARGET)
    assert _notes(badge) == 1, "after the second VKNOTE, notes is %d" % _notes(badge)
    _open_inbox(badge)
    with_note = badge.shot()
    badge.btn("right", "tap")
    state = badge.wait_state(lambda s: s["notes"] == 0, timeout=5)
    assert state["app"] == INBOX, "RIGHT left the Inbox: app is %r" % state["app"]
    emptied = badge.shot(os.path.join(SHOTS, "notify_inbox_empty.png"))
    _not_blank(emptied, "empty Inbox")
    assert emptied != with_note, "the Inbox did not redraw after the note was dismissed"
    # RIGHT and SELECT with nothing listed do nothing.
    badge.btn("right", "tap")
    badge.btn("a", "tap")
    state = badge.state()
    assert state["app"] == INBOX and state["notes"] == 0, "keys on the empty Inbox changed the state: %s" % state

    # 5. A note whose app is not installed: SELECT removes it and the Inbox stays.
    _post(badge, "Third", "no such app", "zz-not-installed")
    assert _notes(badge) == 1, "a note posted while the Inbox is open: notes is %d" % _notes(badge)
    badge.btn("a", "tap")
    state = badge.wait_state(lambda s: s["notes"] == 0, timeout=5)
    assert state["app"] == INBOX, "SELECT on a note with no installed app left the Inbox: %r" % state["app"]

    # 6. Eight at most: the ninth drops the oldest. Then RIGHT empties the list one by one.
    for index in range(MAX_NOTES + 1):
        _post(badge, "Note %d" % index, "body %d" % index, TARGET)
    assert _notes(badge) == MAX_NOTES, "after %d posts, notes is %d" % (MAX_NOTES + 1, _notes(badge))
    badge.btn("down", "tap")
    badge.btn("down", "tap")
    badge.shot(os.path.join(SHOTS, "notify_inbox_full.png"))
    for left in range(MAX_NOTES - 1, -1, -1):
        badge.btn("right", "tap")
        assert _notes(badge) == left, "after a RIGHT, notes is %d, expected %d" % (_notes(badge), left)
    assert badge.state()["app"] == INBOX

    # CANCEL exits.
    _leave(badge, INBOX)
    to_launcher(badge)
