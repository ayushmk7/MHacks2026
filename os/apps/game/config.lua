-- Game: the knobs. main.lua reads all of its behaviour from this table, because the evil game
-- (apps/evilgame) is the same main.lua with another config.lua.
return {
  -- The left header text of every BadgeOS screen, and the game's own name.
  header = "BADGEOS",
  title = "Dodge",

  shop = {
    -- Who is paid: the shop's address, which must have a registry record. This is a deployment
    -- value: the string below is a placeholder that provisioning replaces. Until then every
    -- purchase fails with a message and nothing can be signed.
    recipient = "REPLACE_WITH_SHOP_ADDRESS",

    -- The token paid with; nil is the badge's default token.
    symbol = nil,

    -- What the shop sells. `id` names the item in storage (at most 6 characters); `price` is a
    -- string in display units; `name` is also the memo of the payment.
    --   effect = "life"    one more hit survived in every run
    --   effect = "color"   the player is drawn in `color`, a colour token of the active theme
    items = {
      {id = "shield", name = "shield", price = "5.00", effect = "life", note = "survive one hit per run"},
      {id = "sword", name = "sword", price = "5.00", effect = "life", note = "break one block per run"},
      {id = "paint", name = "green paint", price = "2.00", effect = "color", color = "green", note = "a green ship"},
    },
  },

  -- How fast the blocks fall, in pixels per second: the start, the gain per second survived, the
  -- limit.
  speed = {start = 60, gain = 4, max = 220},

  -- A new block every start_ms, less gain_ms for each second survived, never under min_ms.
  -- aim_percent of the blocks fall straight at the player, so standing still is not a strategy.
  spawn = {start_ms = 900, gain_ms = 15, min_ms = 250, aim_percent = 30},

  -- Sizes in pixels; the player's speed in pixels per second.
  player = {w = 24, h = 8, speed = 160},
  block = {w = 20, h = 12},

  -- Colour tokens of the active theme (badge.theme.color).
  colors = {player = "ink", block = "sub"},

  -- How long the game-over screen stays before the title returns, in ms (a key returns at once).
  over_ms = 3000,

  -- LED flashes: a colour token of the active theme, and the length in ms.
  hit_led = {color = "red", ms = 300},
  bought_led = {color = "green", ms = 600},

  -- Every word on the screen.
  text = {
    play = "Play",
    best = "best %d s",                   -- %d: the high score, seconds survived
    buy = "Buy %s",                       -- %s: the item's name
    shop = "shop",
    owned = "owned",
    hint = "SELECT choose",
    exit = "CANCEL exit",
    seconds = "%s \xC2\xB7 %d s",         -- the header while playing: the title and the score
    lives = "LIVES %d",
    quit = "CANCEL quit",
    over = "GAME OVER",
    survived = "SURVIVED",
    seconds_unit = "SECONDS",
    best_row = "BEST",
    new_best = "new best",
    again = "SELECT title",
    back = "CANCEL back",
    shop_title = "SHOP",
    offer = "Buy %s: %s",                 -- %s: the item's name, then its price and the token
    status = "STATUS",
    bought = "Bought %s",
    not_bought = "Not bought: %s",
    ok = "SELECT ok",
  },

  -- The payment's progress in words (the states of vk.pay).
  states = {
    start = "getting ready",
    record = "checking the shop",
    blockhash = "preparing",
    approve = "approve on the badge",
    submit = "sending",
    confirm = "confirming",
    done = "paid",
    failed = "failed",
  },

  -- A refusal in words (docs/os/reference/reasons.md). Any other reason is shown as it is.
  reasons = {
    cancelled = "you cancelled",
    timeout = "no answer in time",
    not_provisioned = "this badge is not set up",
    bad_arg = "the shop is not set up",
    unsupported = "the wallet is not ready",
    busy = "the wallet is busy",
    unverified = "the shop is not verified",
    revoked = "the shop was revoked",
    expired = "the shop's record expired",
    mismatch = "wrong recipient",
    over_cap = "over the limit",
    no_time = "the clock is not set",
    sign_failed = "the badge could not sign",
  },
}
