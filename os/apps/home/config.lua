-- Home: the knobs. Addresses and the poll period are provisioned settings (rpc_url and
-- balance_poll_s, read with wallet.config), not values in this file.
return {
  -- The menu lists every app the launcher lists (badge.system.launcher_apps()), except Home.
  -- How many menu rows are on screen at once; the menu scrolls to keep the selected one in view.
  menu_rows = 4,

  -- How often the balance is fetched, in seconds, when the config key balance_poll_s has no
  -- usable value. The key wins when it is set; 0 means "fetch once on start, then never".
  default_poll_s = 30,

  -- The longest poll period accepted from the config key, in seconds.
  max_poll_s = 86400,

  -- The left header text of every BadgeOS screen.
  header = "BADGEOS",

  -- What the KEY row says for each wallet.key_location().
  key_text = {se050 = "secure chip", software = "software", none = "none"},

  -- Every other word on the screen.
  text = {
    balance = "BALANCE",
    setup_needed = "SETUP NEEDED",       -- the stub's label on an unprovisioned badge
    unknown_amount = "--",               -- the balance before the first successful fetch
    name = "NAME",
    address = "ADDRESS",
    key = "KEY",
    clock = "CLOCK",
    clock_unset = "--:--",    -- shown while the clock is still syncing
    no_identity = "none",                -- the ADDRESS row on a badge with no key
    no_apps = "no other apps installed",
    thanks = "THANK YOU FOR HACKING",
    open = "SELECT open",
    back = "CANCEL back",
  },

  -- The line under the amount after a balance fetch that did not work. A reason that is not
  -- listed shows nothing.
  fetch_text = {
    no_network = "no Wi-Fi: Settings > Wi-Fi",
    timeout = "node not reachable",
    unsupported = "no token account yet",
  },
}
