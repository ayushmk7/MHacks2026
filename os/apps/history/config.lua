-- History: the knobs.
return {
  -- How many records are read, newest first (the firmware returns at most 64).
  max_entries = 64,

  -- The left header text of every BadgeOS screen.
  header = "BADGEOS",

  -- How often the list is read again while it is showing, in ms (each read is one file read per
  -- record, up to max_entries). It is also read again when a detail screen is closed.
  refresh_ms = 10000,

  -- Rows of an automatic record's detail screen, its signatures included (nine fit the screen).
  detail_rows = 9,

  -- The colour of each outcome: "ok", "warn", "bad" or "mut" (the theme's status inks and its
  -- faint ink). An outcome that is not listed is drawn in the normal ink.
  tone = {
    signed = "ok",
    approved = "ok",
    cancelled = "warn",
    timeout = "warn",
    blocked = "bad",
    failed = "bad",
    received = "ok",
    -- The value colour of the two kinds that are not approvals.
    auto = "mut",
  },

  -- The word shown for each outcome. An outcome that is not listed is shown as the firmware names it.
  outcome_text = {
    signed = "signed",
    approved = "approved",
    cancelled = "cancelled",
    timeout = "timed out",
    blocked = "blocked",
    failed = "failed",
    received = "received",
  },

  -- The row label of a record with no recipient, by signing domain. A domain that is not listed
  -- is shown by its own name.
  domain_label = {
    confirm = "Confirmation",
    ["pay-req"] = "Payment request",
    ["pay-proof"] = "Presence proof",
    contact = "Contact card",
    ["store-reg"] = "Store registration",
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
    reload = "SELECT reload",
    auto_count = "%d signed",          -- an automatic record: how many signatures it holds
    from = "From %s",                  -- a received payment: the payer's short address
    plus = "+",                        -- before a received amount
    kind = "Kind",
    kind_auto = "automatic",
    kind_received = "received",
    what = "What",
    count = "Signatures",
    payer = "From",
    request = "Request",
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
