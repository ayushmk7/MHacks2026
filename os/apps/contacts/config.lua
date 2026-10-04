-- Contacts: the knobs.
return {
  -- The left header text of every BadgeOS screen.
  header = "BADGEOS",

  -- Swap mode: how often this badge broadcasts its HELLO, in ms (the protocol says once a second;
  -- the nonce inside lives for 60 s).
  hello_ms = 1000,

  -- Swap mode: a badge that has not been heard for this long leaves the list, in ms.
  peer_timeout_ms = 4000,

  -- Swap mode: how many nearby badges are listed at once (strongest signal first).
  max_peers = 4,

  -- How long "Saved <name>" or a failure stays on the swap screen, in ms.
  message_ms = 5000,

  -- The LED flash after a contact is saved: a colour token of the active theme, and its length.
  saved_led = {color = "green", ms = 600},

  -- Every word on the screen.
  text = {
    title = "Contacts",
    swap_row = "Swap contacts",
    swap_sub = "find a badge nearby",
    self_named = "self-named",            -- a contact's name is its owner's own choice
    no_date = "",                         -- a contact saved while the clock was not set
    hint_swap = "SELECT swap",
    hint_remove = "RIGHT remove",
    exit = "CANCEL exit",
    confirm_sub = "remove this contact?",
    hint_confirm = "SELECT remove",
    keep = "CANCEL keep",
    swap_title = "SWAP",
    looking = "Looking for badges nearby",
    looking_sub = "open Contacts, Swap on the other badge",
    ask = "Swap with %s?",                -- %s: the name the other badge gave itself
    hint_send = "SELECT send card",
    back = "CANCEL back",
    sent = "card sent",
    sent_to = "Card sent to %s",
    send_failed = "Could not send the card",
    saved = "Saved %s",
  },

  -- A refusal in words (docs/os/reference/reasons.md). Any other reason is shown as it is.
  reasons = {
    bad_arg = "that was not a valid card",
    mismatch = "the card was made for another badge",
    expired = "the swap expired, try again",
    bad_proof = "the card's signature is wrong",
    unsupported = "the contact could not be saved",
    sign_failed = "this badge has no key",
  },

  -- Month names for the date a contact was added.
  months = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"},
}
