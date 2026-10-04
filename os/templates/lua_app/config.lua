-- __NAME__: the knobs. Everything a person might want to change lives here, never in main.lua
-- (docs/os/apps/apps.md, "Rules for every Lua app"). Edit, then push again.
-- Deployment values (addresses, URLs, tokens) are NOT knobs: read them with
-- badge.wallet.config("<key>") from the provisioned config.
return {
  -- The left header text of every BadgeOS screen.
  header = "BADGEOS",

  -- How much one SELECT adds to the count.
  step = 1,

  -- The LED flash on SELECT: a colour token of the active theme, and its length in ms.
  led = {color = "led", ms = 200},

  -- Every word on the screen.
  text = {
    title = "__NAME__",
    count = "Count",
    hint = "SELECT add",
    reset = "Reset",
    reset_sub = "back to zero",
    back = "CANCEL exit",
  },
}
