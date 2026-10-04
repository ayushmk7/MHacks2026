"""The native Wallet app (WP36; apps.md, "Wallet (native)"): five read-only pages and the wallet reset.

Needs one badge with a dev build that has the native runtime (WP31) and the approval engine (WP12).
No network, no hands. The badge must be provisioned for the last check ("the reset was cancelled, so
the badge is still provisioned"); if it is not, the test provisions it with common.provision_test().

In order:
  1. RUN wallet_settings: VKSTATE names it as the running app, native, with no approval open.
  2. One screenshot per page (Status, Tokens, Config, Apps, Reset), RIGHT between them, saved as
     shots/wallet_<n>.png. Each is drawn (more than paper and ink) and no two are the same.
  3. UP/DOWN scroll: on the Config page (more keys than the nine rows a page holds) DOWN changes
     the list and UP puts it back.
  4. On the Reset page SELECT raises the firmware confirmation: title "Change setting", headline
     "ERASE WALLET CONFIG", amber, hold. CANCEL closes it; the badge is still provisioned, the app
     is still running and has drawn its page again over the approval's picture.
  5. CANCEL exits the app.

The pictures are for a person to look at as well: wallet_1.png shows the full public key on two
rows, wallet_3.png every config key by name.
"""

import os
import time

from common import launch, provision_test, to_launcher

_HERE = os.path.dirname(os.path.abspath(__file__))
SHOTS = os.path.join(_HERE, "shots")

APP = "wallet_settings"
PAGES = ("Status", "Tokens", "Config", "Apps", "Reset")
CONFIG_PAGE = 3  # 1-based

WIDTH, HEIGHT = 320, 240
ROW_BYTES = WIDTH * 2
SHOT_BYTES = ROW_BYTES * HEIGHT


def band(shot, y0, y1):
    """Rows y0..y1-1 of a screenshot."""
    return shot[y0 * ROW_BYTES:y1 * ROW_BYTES]


def body(shot):
    """Title and list: everything between the header (y 0..19, whose clock may tick between two
    screenshots) and the footer's rule (y 216)."""
    return band(shot, 20, 216)


def rows(shot):
    """The list alone: nine rows of 18 px from y 41."""
    return band(shot, 41, 203)


def colours(pixels):
    return len({pixels[i:i + 2] for i in range(0, len(pixels), 2)})


def take(badge, name):
    shot = badge.shot(os.path.join(SHOTS, name))
    assert len(shot) == SHOT_BYTES, "%s is %d bytes, expected %d" % (name, len(shot), SHOT_BYTES)
    return shot


def run(badge):
    os.makedirs(SHOTS, exist_ok=True)
    to_launcher(badge)  # also cancels an approval left open by an earlier test
    if badge.info().get("provisioned") != "1":
        provision_test(badge)
    assert badge.state()["provisioned"] is True, "the badge is not provisioned before the test starts"

    # ---- 1. the app starts ------------------------------------------------------------------------
    state = launch(badge, APP)
    assert state["app"] == APP, "VKSTATE app is %r after RUN %s" % (state["app"], APP)
    assert state["native"] is True, "VKSTATE native is %r for %s" % (state["native"], APP)
    assert state["modal"] is False, "an approval is open right after %s started" % APP
    time.sleep(0.3)  # the first frame

    # ---- 2. five pages, five different pictures -----------------------------------------------------
    shots = []
    for number, page in enumerate(PAGES, start=1):
        if number > 1:
            badge.btn("right", "tap")
        shot = take(badge, "wallet_%d.png" % number)
        # Paper and ink at least, and the faint leader dots of a row: a page that drew its rows.
        assert colours(body(shot)) >= 3, "page %d (%s) looks blank: %d colours under the header" % (
            number, page, colours(body(shot)))
        shots.append(shot)
    for first in range(len(shots)):
        for second in range(first + 1, len(shots)):
            assert body(shots[first]) != body(shots[second]), (
                "pages %d (%s) and %d (%s) look the same: LEFT/RIGHT did not change the page" % (
                    first + 1, PAGES[first], second + 1, PAGES[second]))
    assert badge.state()["app"] == APP, "%s stopped while its pages were turned" % APP

    # ---- 3. UP/DOWN scroll the Config page ----------------------------------------------------------
    for _ in range(len(PAGES) - CONFIG_PAGE):
        badge.btn("left", "tap")
    config_top = take(badge, "wallet_3_top.png")
    assert rows(config_top) == rows(shots[CONFIG_PAGE - 1]), (
        "LEFT from the Reset page did not come back to the Config page as it was first drawn")
    badge.btn("down", "tap")
    config_down = take(badge, "wallet_3_down.png")
    assert rows(config_down) != rows(config_top), "DOWN did not scroll the Config page"
    badge.btn("up", "tap")
    config_back = take(badge, "wallet_3_top.png")
    assert rows(config_back) == rows(config_top), "UP did not scroll the Config page back to its top"
    for _ in range(len(PAGES) - CONFIG_PAGE):
        badge.btn("right", "tap")

    # ---- 4. the Reset page: SELECT raises the firmware confirmation; CANCEL leaves everything ------
    reset_page = take(badge, "wallet_5.png")
    assert rows(reset_page) == rows(shots[-1]), "RIGHT from the Config page did not reach the Reset page"
    badge.btn("a", "tap")
    state = badge.wait_state(lambda s: s["modal"] and s["phase"] == "ARMED", timeout=5)
    assert state["headline"] == "ERASE WALLET CONFIG", "headline: %r" % state["headline"]
    assert state["title"] == "Change setting", "title: %r" % state["title"]
    assert state["severity"] == "amber" and state["select"] == "hold", (
        "the reset confirmation is %s / %s, expected amber / hold" % (state["severity"], state["select"]))
    assert state["app"] == APP, "the app behind the confirmation is %r" % state["app"]

    badge.btn("b", "tap")
    state = badge.wait_state(lambda s: not s["modal"], timeout=5)
    assert state["provisioned"] is True, "VKSTATE provisioned is %r after a cancelled reset" % state["provisioned"]
    assert badge.info().get("provisioned") == "1", "VKINFO provisioned is not 1 after a cancelled reset"
    assert state["app"] == APP, "the app is %r after the confirmation closed, expected %s" % (state["app"], APP)
    # The CANCEL that closed the confirmation must not reach the app, and the app must paint its own
    # page again: the canvas still held the approval's picture when it was resumed.
    time.sleep(0.5)
    assert badge.state()["app"] == APP, "the CANCEL that closed the confirmation also closed the app"
    after = take(badge, "wallet_5_after_cancel.png")
    assert body(after) == body(reset_page), (
        "after the confirmation closed the Reset page was not drawn again as before")

    # ---- 5. CANCEL exits ------------------------------------------------------------------------------
    badge.btn("b", "tap")
    badge.wait_state(lambda s: s["app"] != APP, timeout=5)
    to_launcher(badge)
