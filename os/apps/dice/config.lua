-- Dice: the knobs. main.lua reads everything a person might want to change from this table.
return {
  -- The left header text of every BadgeOS screen.
  header = "BADGEOS",

  -- How many dice a roll throws when the app starts, and the most UP can raise it to.
  -- (The least is one. More dice than fit the screen are drawn smaller.)
  count = 2,
  max = 5,

  -- The roll: how long the dice tumble before they settle, and how often a tumbling face
  -- changes, in ms. roll_ms 0 settles at once.
  roll_ms = 600,
  tick_ms = 60,

  -- How often the screen is drawn again when nothing is rolling, in ms: only the header's clock
  -- and battery can have changed.
  idle_ms = 1000,

  -- How many past rolls are kept, newest first, 1 to 20 (as many as fit are shown: five with the
  -- default die size).
  history = 5,

  -- A die, in pixels: the side of its square face, the gap to the next die, the corner radius,
  -- the thickness of its edge. The pip's radius is the side divided by pip_div.
  die = {size = 52, gap = 8, radius = 7, edge = 2, pip_div = 10},

  -- Colour tokens of the active theme (badge.theme.color): ink on paper. idle_pip is the colour
  -- of the pips before the first roll and after the number of dice changed, when the faces are
  -- not a result.
  colors = {face = "paper", edge = "ink", pip = "ink", idle_pip = "faint"},

  -- The LED flash on a roll: a colour token of the active theme, and the length in ms.
  led = {color = "led", ms = 400},

  -- Every word on the screen.
  text = {
    title = "DICE",
    total = "TOTAL",
    unit_one = "1 DIE",
    unit_many = "%d DICE",              -- %d: how many dice
    no_total = "--",                    -- before the first roll, and after the count changed
    rolling = "..",                     -- in place of the total while the dice tumble
    roll = "ROLL %d",                   -- a history row's label; %d: the roll's number
    empty = "SELECT to roll",           -- in place of the history before the first roll
    hint = "SELECT roll \xC2\xB7 UP/DOWN dice",
    exit = "CANCEL exit",
  },
}
