-- History: the knobs.
return {
  -- How many records are read, newest first (the firmware returns at most 64).
  max_entries = 64,

  -- The left header text of every BadgeOS screen.
  header = "BADGEOS",

  -- The colour of each outcome: "ok", "warn", "bad" or "mut" (the theme's status inks and its
  -- faint ink). An outcome that is not listed is drawn in the normal ink.
  tone = {
    signed = "ok",
    approved = "ok",
    cancelled = "warn",
    timeout = "warn",
    blocked = "bad",
    failed = "bad",
  },

  -- The word shown for each outcome. An outcome that is not listed is shown as the firmware names it.
  outcome_text = {
    signed = "signed",
    approved = "approved",
    cancelled = "cancelled",
    timeout = "timed out",
    blocked = "blocked",
    failed = "failed",
  },

  -- The words for each reason code (docs/os/reference/reasons.md). A code that is not listed is
  -- shown as it is.
  reason_text = {
    ok = "ok",
    cancelled = "cancelled by you",
    timeout = "not answered in time",
    undecodable = "payment could not be read",
    unverified = "recipient not verified",
    revoked = "recipient revoked",
    expired = "record expired",
    mismatch = "recipient or amount differs",
    bad_proof = "bad presence proof",
    over_cap = "over the limit",
    no_time = "clock not set",
    sign_failed = "the key did not sign",
  },

  -- The row label of a record with no recipient, by signing domain. A domain that is not listed
  -- is shown by its own name.
  domain_label = {
    confirm = "Confirmation",
  },

  -- Every other word on the screens.
  text = {
    title = "History",
    detail_title = "Record",
    empty = "No signatures yet",
    no_time = "--:--",                 -- a record made while the clock had no source
    no_date = "clock was not set",
    no_app = "system",                 -- a record no app asked for
    none = "-",                        -- an empty field in the detail view
    dev_mark = " (dev override)",
    utc = " UTC",
    details = "SELECT details",
    next = "UP/DOWN next",
    back = "CANCEL back",
    time = "Time",
    outcome = "Outcome",
    reason = "Reason",
    amount = "Amount",
    to = "To",
    address = "Address",
    app = "App",
    domain = "Domain",
    signature = "Signature",
  },
}
