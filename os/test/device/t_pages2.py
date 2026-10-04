"""Settings pages, group 2 (WP37; shell.md, "Settings pages"): App store, Identity, New identity,
Display, LEDs, Device info, Console, and the action rows Wallet and Inbox.

Needs one badge with a dev build that has the BadgeOS shell with all thirteen settings rows (the
boot line says pages=13) and the native apps wallet_settings and inbox. No network, no hands.

The test navigates by VKSTATE's `screen` field, never by comparing screenshots. Screenshots are
saved as shots/shell_<screen>_<theme>.png for a person to look at.

In order:
  1. A reset: the launcher's cursor is on the first cell and the inbox (notes live in RAM) is empty.
  2. store, identity, display, leds, info, console: from the launcher CANCEL opens `settings`, DOWN
     to the row, SELECT opens the page (VKSTATE screen == id), the screenshot is not uniform,
     CANCEL returns to `settings`.
  3. identity: SELECT on the page opens `identity_new`; it is left with CANCEL only.
  4. display: LEFT and RIGHT each change the picture and the screen stays `display`; the
     brightness is put back (LEFT then RIGHT, or RIGHT then LEFT when it was at the floor).
  5. Wallet row: SELECT launches wallet_settings; to_launcher returns to screen `launcher`.
  6. Inbox row: with one note posted (VKNOTE) the Settings list and the launcher differ from the
     empty case; SELECT launches inbox; the note is dismissed there, so the inbox ends empty.

NEVER send SELECT (btn a) while VKSTATE screen is "identity_new": it raises the confirmation that
erases the badge's key, and with it the badge ID and the wallet address.

The test resets the badge, so the clock is unset afterwards. It assumes the Settings list puts its
cursor on the first row each time it is opened from the launcher (shell.md: enter() resets state).
"""

import os
import time

from common import HOLD_MARGIN_MS, hold_ms, to_launcher

_HERE = os.path.dirname(os.path.abspath(__file__))
SHOTS = os.path.join(_HERE, "shots")

WIDTH, HEIGHT = 320, 240
ROW_BYTES = WIDTH * 2
SHOT_BYTES = ROW_BYTES * HEIGHT

# The Settings list, in order (execution-plan.md, section 5.3). A row's index is the number of
# DOWN taps from the top.
ROWS = ["theme", "wifi", "bluetooth", "espnow", "push", "store", "identity", "display", "leds",
        "wallet", "inbox", "info", "console"]
PAGES = ["store", "identity", "display", "leds", "info", "console"]


def body(shot):
    """Everything between the header (y 0..19, whose clock and battery text may change between two
    screenshots) and the footer's rule (y 216)."""
    return shot[20 * ROW_BYTES:216 * ROW_BYTES]


def take(badge, name):
    shot = badge.shot(os.path.join(SHOTS, name))
    assert len(shot) == SHOT_BYTES, "%s is %d bytes, expected %d" % (name, len(shot), SHOT_BYTES)
    colours = {shot[i:i + 2] for i in range(0, len(shot), 2)}
    assert len(colours) > 1, "%s: the screen is one flat colour" % name
    return shot


def on_screen(badge, name, timeout=5):
    """Waits until the shell shows screen `name` with no app running."""
    try:
        return badge.wait_state(lambda s: s["app"] == "" and s.get("screen") == name, timeout=timeout)
    except Exception as exc:  # the tool's timeout error does not say what was expected
        raise AssertionError("screen '%s' did not appear: %s; state: %s" % (name, exc, badge.state()))


def goto_row(badge, row_id):
    """Opens Settings afresh from the launcher and moves the cursor to the row."""
    to_launcher(badge)
    on_screen(badge, "launcher")
    badge.btn("b", "tap")  # CANCEL on the launcher opens Settings
    on_screen(badge, "settings")
    for _ in range(ROWS.index(row_id)):
        badge.btn("down", "tap")


def await_app(badge, app_id, timeout=10):
    """After SELECT on an action row: the app is running. Answers the first-run consent prompt."""
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
        assert time.monotonic() < deadline, "SELECT on the row did not start %s; state: %s" % (app_id, state)
        time.sleep(0.1)


def check_identity(badge, theme):
    badge.btn("a", "tap")  # SELECT on `identity` opens the warning screen; it changes nothing yet
    on_screen(badge, "identity_new")
    take(badge, "shell_identity_new_%s.png" % theme)
    # WARNING: leave `identity_new` with CANCEL (btn b) ONLY. NEVER send SELECT (btn a) here: it
    # raises the hold-SELECT confirmation that erases the badge's key, and with the key go the badge
    # ID and the wallet address every other test and the registry know this badge by.
    badge.btn("b", "tap")
    on_screen(badge, "identity")


def check_display(badge, first):
    """LEFT and RIGHT each change the picture; the brightness is left as found."""
    badge.btn("left", "tap")
    on_screen(badge, "display")
    after_one = badge.shot()
    if body(after_one) != body(first):
        back_key = "right"
    else:
        # Already at the floor (8): LEFT does nothing there. Go up first, then back down.
        badge.btn("right", "tap")
        on_screen(badge, "display")
        after_one = badge.shot()
        assert body(after_one) != body(first), "neither LEFT nor RIGHT changed the Display page"
        back_key = "left"
    badge.btn(back_key, "tap")
    on_screen(badge, "display")
    after_two = badge.shot()
    assert body(after_two) != body(after_one), "%s did not change the Display page" % back_key.upper()


def run(badge):
    os.makedirs(SHOTS, exist_ok=True)

    # 1. A known start: launcher cursor on the first cell, no notification waiting.
    badge.reset()
    to_launcher(badge)
    on_screen(badge, "launcher")
    theme = badge.ok("VKGET theme") or "receipt-light"

    # 2 to 4. The six pages.
    for page in PAGES:
        goto_row(badge, page)
        badge.btn("a", "tap")  # SELECT opens the page
        on_screen(badge, page)
        shot = take(badge, "shell_%s_%s.png" % (page, theme))
        if page == "identity":
            check_identity(badge, theme)
        elif page == "display":
            check_display(badge, shot)
        badge.btn("b", "tap")  # CANCEL
        on_screen(badge, "settings")

    # 5. Wallet row: an action row that launches the native Wallet app.
    goto_row(badge, "wallet")
    badge.btn("a", "tap")
    state = await_app(badge, "wallet_settings")
    assert state.get("screen", "") == "", "an app is running but screen is '%s'" % state.get("screen")
    to_launcher(badge)
    on_screen(badge, "launcher")

    # 6. Inbox row. The empty case first: the launcher scrolled to its last rows (UP from the first
    # row wraps; the native apps, inbox among them, are listed last) and the Settings list with the
    # cursor on the Inbox row.
    assert badge.state()["notes"] == 0, "the inbox is not empty after a reset"
    badge.btn("up", "tap")
    on_screen(badge, "launcher")
    launcher_empty = take(badge, "shell_launcher_inbox0_%s.png" % theme)
    badge.btn("b", "tap")
    on_screen(badge, "settings")
    for _ in range(ROWS.index("inbox")):
        badge.btn("down", "tap")
    settings_empty = take(badge, "shell_settings_inbox0_%s.png" % theme)

    reply = badge.cmd("VKNOTE Pages test|from t_pages2|hello_native")[-1]
    assert reply == "OK", "VKNOTE -> %s" % reply
    badge.wait_state(lambda s: s["notes"] == 1, timeout=5)
    time.sleep(1.3)  # the Settings list repaints every 1000 ms
    settings_note = take(badge, "shell_settings_inbox1_%s.png" % theme)
    assert body(settings_note) != body(settings_empty), "the Inbox row does not show the waiting note"

    badge.btn("b", "tap")  # back to the launcher, cursor where it was
    on_screen(badge, "launcher")
    time.sleep(0.3)
    launcher_note = take(badge, "shell_launcher_inbox1_%s.png" % theme)
    assert body(launcher_note) != body(launcher_empty), "the launcher's inbox cell does not show the waiting note"

    goto_row(badge, "inbox")
    badge.btn("a", "tap")
    await_app(badge, "inbox")
    badge.btn("right", "tap")  # RIGHT in the Inbox dismisses the note: the inbox ends empty
    badge.wait_state(lambda s: s["notes"] == 0, timeout=5)

    to_launcher(badge)
    on_screen(badge, "launcher")
