#!/usr/bin/env python3
"""R1 SE050 survey: badge -> pubkey -> key location -> failing step.

The serial log is parsed by this script; Settings > Identity is read by hand.
Never enter a private key or seed phrase - only the public key shown on screen.

  python harness/se050_survey.py [--port /dev/cu.usbserial-10] [--badges N]
      (without --badges it asks how many badges you have)
  python harness/se050_survey.py --parse LOGFILE   # assess one saved log
  python harness/se050_survey.py --check           # self-test, no badge needed

With --port the script captures each badge's boot log itself: you power-cycle
the badge (unplug USB, switch off/on, replug) and it records the cold boot. It
never resets the badge, because a warm reset can wedge the I2C bus. Needs
pyserial from harness/requirements.txt. Without --port, give the path to a log
you captured with your own serial monitor, or Enter for none.
"""

import csv
import hashlib
import json
import re
import sys
import time
from datetime import datetime, timezone
from pathlib import Path

B58_ALPHABET = "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz"
BADGE_ID = re.compile(r"[1-9A-HJ-NP-Za-km-z]{8}\Z")

# What Settings > Identity prints next to "key lives in", mapped to the
# keyLocation vocabulary dashboard/server/config/badges.json accepts.
SCREEN_LOCATIONS = {"secure element": "se050", "software": "software", "unknown": "unknown"}

FIELDS = ["badge_id", "pubkey", "settings_key_location", "log_key_location", "log_state",
          "failure_step", "failure_detail", "i2c_bus", "serial_log_reference",
          "serial_log_sha256", "result"]

CAPTURE_SECONDS = 15

# ---------------------------------------------------------------------------
# Serial-log assessment
#
# Mirrors firmware src/identity/identity.cpp and src/hal/se050_apdu.cpp. On a
# badge's FIRST boot identity::begin() creates the key: on the SE050 it brings
# up the applet, checks the key object exists (stage "exists"), generates it
# ("generate"), reads the public key ("read"), then signs a probe ("sign") and
# verifies it locally. Every later boot only LOADS the stored identity and runs
# no probe, so a later-boot log can say where the key lives but not that the
# probe passed - that is "unknown", not a pass.
# ---------------------------------------------------------------------------

ID_FINAL = re.compile(r"\[id\] ([1-9A-HJ-NP-Za-km-z]{8}), (secure element|software) \(\d+ ms\)")
ID_REFUSED = re.compile(r"\[id\] SE050 refused at (\w+)")
ID_UNAVAILABLE = re.compile(r"\[id\] SE050 unavailable \(([^)]*)\)")
SE_SIGN_ERR = re.compile(r"\[se050\] sign(: transport failed| refused)")

STAGE_TO_STEP = {
    "select": "se050_presence_or_atr",
    "exists": "other_documented_step",  # the object-exists check before generation
    "generate": "key_generation",
    "read": "public_key_read",
    "sign": "eddsa_sign",
}


def assess_log(text):
    """Returns dict(log_state, failure_step, failure_detail, log_key_location,
    log_badge_id, i2c_bus) from one boot log."""
    out = dict(log_state="unknown", failure_step="unknown", failure_detail="",
               log_key_location="", log_badge_id="", i2c_bus="unknown")

    # Bus health: after a warm reset (or with the power switch off) the badge can
    # boot with SCL clamped low, and every SE050 step then fails for a reason that
    # is not the SE050. See docs/logs/BADGE-BUTTONS-I2C.md.
    if "SCL(GPIO9)=LOW" in text or "TCA9534 init FAILED" in text:
        out["i2c_bus"] = "down"
    elif "SCL(GPIO9)=HIGH" in text:
        out["i2c_bus"] = "ok"

    final = ID_FINAL.search(text)
    if final:
        out["log_badge_id"] = final.group(1)
        out["log_key_location"] = "se050" if final.group(2) == "secure element" else "software"

    created = "falling back to a software key" in text or "generating Ed25519 key" in text

    def failed(step, detail):
        out.update(log_state="failed", failure_step=step, failure_detail=detail)
        return out

    if "[id] no identity" in text:
        return failed("identity_selection", "no identity: SE050 and software both failed")
    if "SE050 public key does not match stored identity" in text:
        return failed("identity_selection", "SE050 key does not match the stored identity")

    m = ID_UNAVAILABLE.search(text)
    if m:
        return failed("se050_presence_or_atr", f"SE050 unavailable ({m.group(1)})")
    if "SE050 did not answer; identity present" in text:
        return failed("se050_presence_or_atr", "stored SE050 identity, part did not answer")
    if "SE050 out of time before key generation" in text:
        return failed("key_generation", "SE050 time budget ran out before generation")
    m = ID_REFUSED.search(text)
    if m:
        stage = m.group(1)
        detail = f"SE050 refused at {stage}"
        return failed(STAGE_TO_STEP.get(stage, "other_documented_step"), detail)
    if "SE050 answered but its public key could not be read" in text:
        return failed("public_key_read", "stored SE050 identity, public key read failed")
    if "SE050 signature failed local verification" in text:
        if SE_SIGN_ERR.search(text):
            return failed("eddsa_sign", "SE050 refused or dropped the probe signature")
        return failed("probe_signature_verification", "SE050 probe signature did not verify")

    if not final:
        out["failure_detail"] = "no [id] line in log"
        return out

    if out["log_key_location"] == "se050" and created:
        # Created on the SE050 this boot: generate/read/sign/verify all passed.
        out.update(log_state="probe_passed", failure_step="none")
    elif out["log_key_location"] == "se050":
        out["failure_detail"] = "stored SE050 identity loaded; probe not run this boot"
    elif not created:
        out["failure_detail"] = "stored software identity loaded; original fallback reason not in this log"
    else:
        out["failure_detail"] = "fell back to software; no SE050 step error logged"
    return out


# ---------------------------------------------------------------------------
# Keys
# ---------------------------------------------------------------------------

def b58decode(value):
    number = 0
    for ch in value:
        index = B58_ALPHABET.find(ch)
        if index < 0:
            raise ValueError(f"not base58: {ch!r}")
        number = number * 58 + index
    raw = number.to_bytes((number.bit_length() + 7) // 8, "big") if number else b""
    pad = len(value) - len(value.lstrip("1"))
    return b"\x00" * pad + raw


def valid_pubkey(value):
    try:
        return 43 <= len(value) <= 44 and len(b58decode(value)) == 32
    except ValueError:
        return False


def valid_badge_id(value):
    return bool(BADGE_ID.fullmatch(value))


# ---------------------------------------------------------------------------
# Serial capture
# ---------------------------------------------------------------------------

def capture_boot_log(port, destination):
    """Captures a COLD boot. The script never resets the badge itself: a warm
    reset (EN pulse with the rails still up) can leave the I2C bus clamped low
    until the next full power cycle, which would spoil the very log it captures.
    Instead it waits for the user to power-cycle, opens the port the moment it
    reappears, and records the boot. The firmware waits ~1.2 s for USB before
    logging, and the [id] / [i2c] lines come seconds later, so they are caught."""
    try:
        import serial  # pyserial
    except ImportError:
        sys.exit("--port needs pyserial: harness/.venv/bin/pip install -r harness/requirements.txt")
    device = Path(port)
    print("  Now: unplug USB, power switch OFF, power switch ON, plug USB back in.")
    while device.exists():
        time.sleep(0.05)
    print("  USB unplugged - waiting for the badge to come back...")
    while not device.exists():
        time.sleep(0.02)
    link = serial.Serial()
    link.port, link.baudrate, link.timeout = port, 115200, 0.2
    link.dtr = link.rts = False  # leave EN and GPIO0 alone: just listen
    for _ in range(50):  # the node can exist a moment before it opens
        try:
            link.open()
            break
        except serial.SerialException:
            time.sleep(0.02)
    else:
        sys.exit(f"could not open {port}")
    try:
        data = b""
        end = time.time() + CAPTURE_SECONDS
        while time.time() < end:
            data += link.read(4096)
    finally:
        link.close()
    destination.write_bytes(data)
    return destination


# ---------------------------------------------------------------------------
# Survey
# ---------------------------------------------------------------------------

def prompt(label, choices):
    allowed = "/".join(choices)
    while True:
        value = input(f"{label} [{allowed}]: ").strip().lower()
        if value in choices:
            return value
        print("Choose one of the listed values.")


def arg_value(flag):
    if flag not in sys.argv:
        return None
    try:
        return sys.argv[sys.argv.index(flag) + 1]
    except IndexError:
        sys.exit(f"{flag} needs a value")


def new_run_dir():
    out = Path(__file__).resolve().parent / "results"
    stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    run = out / stamp
    suffix = 1
    while run.exists():
        run = out / f"{stamp}-{suffix}"
        suffix += 1
    run.mkdir(parents=True)
    return run


def survey_badge(number, total, run, port):
    print(f"\nBadge {number}/{total}")

    log_path = None
    log_state_override = None
    if port:
        input(f"Connect badge {number} on {port}, then press Enter to capture "
              f"{CAPTURE_SECONDS}s of its cold-boot log...")
        log_path = capture_boot_log(port, run / f"badge{number}-boot.log")
        print(f"Captured {log_path}")
    else:
        ref = input("Serial boot log path (Enter if none): ").strip()
        if ref:
            try:
                log_path = Path(ref).expanduser().resolve(strict=True)
                if not log_path.is_file():
                    raise OSError("not a file")
            except (OSError, ValueError):
                log_path = None
                log_state_override = "unreadable"
                print("Log could not be read; recording unreadable, not success.")
        else:
            log_state_override = "unavailable"

    if log_path:
        raw = log_path.read_bytes()
        log = assess_log(raw.decode("utf-8", errors="replace"))
        log_hash = hashlib.sha256(raw).hexdigest()
    else:
        log = assess_log("")
        log.update(log_state=log_state_override, failure_detail="")
        log_hash = ""

    print(f"  log: {log['log_state']}, step {log['failure_step']}"
          + (f" ({log['failure_detail']})" if log["failure_detail"] else "")
          + f", I2C bus {log['i2c_bus']}")
    if log["i2c_bus"] == "down":
        print("  WARNING: I2C bus was down in this log (SCL held low). SE050 results are not")
        print("  trustworthy - unplug USB, power-cycle the switch, and survey this badge again.")

    while True:
        pubkey = input("Settings > Identity public key (full base58, both lines): ").strip().replace(" ", "")
        if valid_pubkey(pubkey):
            break
        print("Not a 32-byte base58 public key (expected 43-44 characters).")
    badge_id = pubkey[:8]
    print(f"  badge ID {badge_id} - check it matches the BADGE ID on screen.")
    if log["log_badge_id"] and log["log_badge_id"] != badge_id:
        print(f"  WARNING: the log is from badge {log['log_badge_id']}, not {badge_id}.")

    screen = SCREEN_LOCATIONS[prompt("Settings > Identity 'key lives in'", list(SCREEN_LOCATIONS))]
    if log["log_key_location"] and screen != "unknown" and screen != log["log_key_location"]:
        print(f"  WARNING: screen says {screen}, log says {log['log_key_location']}.")

    if log["i2c_bus"] == "down":
        result = "unknown/incomplete (I2C bus down during log)"
    elif screen == "software":
        result = "fallback to NVS"
    elif screen == "se050" and log["log_state"] == "probe_passed":
        result = "SE050 verified (screen + log probe)"
    elif screen == "se050":
        result = "SE050 per screen; probe not verified"
    else:
        result = "unknown/incomplete"

    return dict(badge_id=badge_id, pubkey=pubkey, settings_key_location=screen,
                log_key_location=log["log_key_location"], log_state=log["log_state"],
                failure_step=log["failure_step"], failure_detail=log["failure_detail"],
                i2c_bus=log["i2c_bus"], serial_log_reference=str(log_path or ""),
                serial_log_sha256=log_hash, result=result)


def write_results(run, rows):
    with (run / "results.csv").open("w", newline="", encoding="utf-8") as f:
        writer = csv.DictWriter(f, fieldnames=FIELDS)
        writer.writeheader()
        writer.writerows(rows)

    # Handoff to U: the fields dashboard/server/config/badges.json takes per badge.
    handoff = [{"pubkey": r["pubkey"], "keyLocation": r["settings_key_location"],
                "badgeId": r["badge_id"]} for r in rows]
    (run / "pubkeys-for-U.json").write_text(json.dumps(handoff, indent=2) + "\n", encoding="utf-8")

    lines = ["| Badge | Pubkey | Key location | Log | Failing step | Result |",
             "| --- | --- | --- | --- | --- | --- |"]
    for r in rows:
        step = r["failure_step"] + (f" ({r['failure_detail']})" if r["failure_detail"] else "")
        lines.append(f"| {r['badge_id']} | `{r['pubkey']}` | {r['settings_key_location']} | "
                     f"{r['log_state']} | {step} | {r['result']} |")
    (run / "table.md").write_text("\n".join(lines) + "\n", encoding="utf-8")


def self_check():
    assert valid_badge_id("7Qk2M9xA") and not valid_badge_id("0Qk2M9xA")
    assert valid_pubkey("Gn2GQYmcQkatE2594ov2ELKkYWZHwARG7FiAoPWSEcxq")
    assert not valid_pubkey("Gn2GQYmcQkatE2594ov2ELKkYWZHwARG7FiAoPWSEcx0")
    assert len(b58decode("11111111111111111111111111111111")) == 32

    def case(log, state, step):
        got = assess_log(log)
        assert (got["log_state"], got["failure_step"]) == (state, step), (log, got)

    case("[id] 5mG1mVHD, software (1 ms)", "unknown", "unknown")
    case("[id] generating Ed25519 key 0x7fff0201 in SE050\n[id] 5mG1mVHD, secure element (900 ms)",
         "probe_passed", "none")
    case("[id] 5mG1mVHD, secure element (40 ms)", "unknown", "unknown")
    case("[id] SE050 unavailable (select)\n[id] falling back to a software key\n"
         "[id] 5mG1mVHD, software (30 ms)", "failed", "se050_presence_or_atr")
    case("[id] SE050 refused at generate, sw 6985\n[id] falling back to a software key",
         "failed", "key_generation")
    case("[id] SE050 refused at read, sw 6a82", "failed", "public_key_read")
    case("[se050] sign refused, sw 6985\n[id] SE050 signature failed local verification, not trusting it",
         "failed", "eddsa_sign")
    case("[id] SE050 signature failed local verification, not trusting it",
         "failed", "probe_signature_verification")
    case("[id] SE050 public key does not match stored identity; refusing to sign with it\n"
         "[id] 5mG1mVHD, secure element (40 ms)", "failed", "identity_selection")
    case("nothing useful", "unknown", "unknown")
    assert assess_log("[i2c] scanning bus... SDA(GPIO10)=HIGH SCL(GPIO9)=LOW")["i2c_bus"] == "down"
    print("survey validation checks passed")


def main():
    if "--check" in sys.argv:
        self_check()
        return

    parse = arg_value("--parse")
    if parse:
        print(json.dumps(assess_log(Path(parse).read_text(errors="replace")), indent=2))
        return

    port = arg_value("--port")
    print("R1 survey. Read Settings > Identity on each badge. Never enter a private key or seed phrase.")

    # Survey only the badges actually in hand: --badges N, or asked up front.
    if arg_value("--badges"):
        try:
            total = int(arg_value("--badges"))
        except ValueError:
            sys.exit("--badges needs a number")
    else:
        while True:
            try:
                total = int(input("How many badges are you surveying now? ").strip())
            except ValueError:
                total = 0
            except (KeyboardInterrupt, EOFError):
                sys.exit("\nStopped.")
            if total >= 1:
                break
            print("Enter a number, 1 or more.")
    if total < 1:
        sys.exit("--badges must be at least 1")

    run = new_run_dir()
    rows = []
    try:
        for number in range(1, total + 1):
            rows.append(survey_badge(number, total, run, port))
    except (KeyboardInterrupt, EOFError):
        print("\nStopped.")
    finally:
        # Partial runs are kept: an interrupted survey still saves what it has.
        if rows:
            write_results(run, rows)
            print(f"\nSaved {len(rows)} badge(s) to {run}/ (results.csv, table.md, pubkeys-for-U.json)")
            print("The R1 table in the spec covers all 4 badges; survey the rest in later runs.")
        elif not any(run.iterdir()):
            run.rmdir()  # nothing surveyed and no log captured: leave no empty folder


if __name__ == "__main__":
    main()
