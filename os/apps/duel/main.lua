-- Duel (docs/os/apps/apps.md, "Duel"): two badges, one stake.
--
--   1. Invite.  A picks a stake and broadcasts INVITE. B shows "Duel <name> for <stake>?";
--               SELECT sends ACCEPT.
--   2. Play.    A reaction round: A chooses a random delay and sends it as GO; both screens flash
--               when it has passed; each player presses SELECT; each badge sends its reaction
--               time as TIME. Best of config.rounds; a tied round is replayed.
--   3. Settle.  The winner opens a payment request for the stake (wallet.request_open). The loser
--               finds the request whose payee is the winner's key in wallet.requests() and pays
--               it with vk.pay: the ordinary payment flow, with the firmware approval.
--   4. Result.  The winner shows PAID only after vk.receive has fetched the transaction a RESULT
--               names and the firmware has checked that it pays this request (amount, account,
--               memo, signature), or "unpaid" when none does within config.settle_timeout_s.
--
-- Stated honestly: there is no escrow (the loser can press CANCEL), and reaction times are
-- self-reported (a modified app could lie). The firmware approval shows the true amount and the
-- verified recipient whatever this app draws.
--
-- Frames (protocol/espnow.md: app types 64 to 71 belong to Duel), built and matched only with
-- vk.app_frame and vk.app_body. `game` is the 8 random bytes the inviter drew.
--   64 INVITE   game(8) inviter's public key(32) stake (text, display units)     broadcast
--   65 ACCEPT   game(8) accepter's public key(32)                                unicast
--   66 GO       game(8) round(1) ms until the flash (u16, big-endian)            unicast, inviter
--   67 TIME     game(8) round(1) reaction ms (u16; 65535 = pressed too early)    unicast
--   68 LEAVE    game(8)                                                          unicast
-- The public keys are how the loser recognises the winner's request. Each badge times itself
-- from its own flash, so a GO that arrives late is fair: GO carries the time still to wait.
--
-- Every state but the title has a timeout back to the title. CANCEL leaves the state (and the
-- duel) from anywhere, and exits the app from the title.
--
-- Log lines (each "[app] DUEL ..."), for test/device/t_app_duel.py and t_duel_2.py:
--   up ch <channel>                          once, on start
--   title                                    the title screen is showing
--   stake <text>                             a stake was chosen
--   not ready <reason>                       not_provisioned or no_time: no duel can be settled
--   invite <game hex> / invite timeout
--   invited <stake> <mac> / accept <game hex> / accepted <mac>
--   go <round> <ms> / flash <round> / time <round> <ms>
--   round <round> <mine> <theirs> <won|lost|tie>
--   over <won|lost|draw> <my wins>-<their wins>
--   request <req_id> / request err <reason>          winner
--   result <status> / paid <short signature> / unpaid <why>   winner
--   paying <req_id> / pay <state> / pay done <signature> / pay failed <reason>   loser
--   left / opponent left / timeout <state>

local cfg = require("config")
local vk = require("vk")
local ui = vk.ui
local gfx, wallet, espnow, codec = badge.gfx, badge.wallet, badge.espnow, badge.codec

local T_INVITE, T_ACCEPT, T_GO, T_TIME, T_LEAVE = 64, 65, 66, 67, 68
local FALSE_START = 0xFFFF
local BODY_COLS = (ui.BODY_X1 - ui.BODY_X0) // 6
local TITLE_Y, BODY_Y, LINE_PITCH = 30, 56, 12

-- States in which the other badge is told when this one leaves.
local TELL = {starting = true, armed = true, flash = true, wait_time = true, round_result = true,
              request = true, await_pay = true, find = true, paying = true}
-- States that end in "the other badge left" when it says so.
local PLAYING = {starting = true, armed = true, flash = true, wait_time = true, round_result = true,
                 find = true}

local PAY_WORDS = {
  presence = "Checking presence.", record = "Fetching the record.", blockhash = "Building.",
  approve = "Approve on the firmware screen.", submit = "Sending.", confirm = "Confirming.",
}

local state = "title"
local deadline = nil            -- millis at which the state times out
local next_at = 0               -- millis of the state's next repeated action
local note = ""                 -- the title screen's message line
local stake_i = 1
local symbol = ""

local game, role, opp, stake    -- the duel: 8 bytes, "inviter" | "guest", {mac, address, name}, text
local offer                     -- an invitation being shown: {game, mac, address, name, stake}
local round, wins_me, wins_them = 0, 0, 0
local mine, theirs = {}, {}     -- reaction times by round
local go_at, pause_until = 0, 0
local last                      -- the round just played: {mine, theirs, verdict}
local request, watch            -- winner: the open request and its vk.receive check
local flow, pay_state           -- loser: the vk.pay flow and its last state
local outcome = {title = "", lines = {}}

local function now() return badge.millis() end
local function log(text) badge.log("DUEL " .. text) end
local function u16(n) return string.char(n // 256, n % 256) end

-- A decimal string without leading or trailing zeros ("05.50" -> "5.5"), or nil if it is not one.
local function norm(amount)
  local whole, frac = tostring(amount or ""):match("^(%d+)%.?(%d*)$")
  if not whole then return nil end
  whole = whole:gsub("^0+(%d)", "%1")
  frac = frac:gsub("0+$", "")
  return frac == "" and whole or whole .. "." .. frac
end

local function wrap(text, width)
  local lines, line = {}, ""
  for word in tostring(text):gmatch("%S+") do
    if line == "" then
      line = word
    elseif #line + 1 + #word <= width then
      line = line .. " " .. word
    else
      lines[#lines + 1] = line
      line = word
    end
  end
  if line ~= "" then lines[#lines + 1] = line end
  return lines
end

local function random_between(low, high)
  local bytes = badge.system.random_bytes(2)
  return low + (bytes:byte(1) * 256 + bytes:byte(2)) % (high - low + 1)
end

local pulse = vk.led_pulse      -- the LEDs in a theme colour

local function send(type_, body, mac)
  local frame = vk.app_frame(type_, body)
  if not frame then return end
  if mac then espnow.send(mac, frame) else espnow.broadcast(frame) end
end

-- What the other badge calls itself over ESP-NOW (a claim), or nil.
local function peer_name(mac)
  for _, peer in ipairs(espnow.peers() or {}) do
    if peer.mac == mac and type(peer.name) == "string" and peer.name ~= "" then return peer.name end
  end
  return nil
end

local function time_text(ms)
  if ms == FALSE_START then return "TOO EARLY" end
  if ms >= cfg.reaction_max_ms then return "NO PRESS" end
  return ms .. " ms"
end

local function seconds_left()
  if not deadline then return "" end
  return math.max(0, (deadline - now()) // 1000) .. " s"
end

local function enter(name, timeout_ms)
  state = name
  vk.keep_awake(name ~= "title" and name ~= "result")   -- a duel in progress does not dim
  deadline = timeout_ms and now() + timeout_ms or nil
  next_at = 0
end

local function close_request()
  if request then wallet.request_close(request.req_id) end
  request, watch = nil, nil
end

local function to_title(text)
  close_request()
  game, role, opp, stake, offer, flow, pay_state, last = nil, nil, nil, nil, nil, nil, nil, nil
  round, wins_me, wins_them, mine, theirs = 0, 0, 0, {}, {}
  note = text or ""
  enter("title")
  log("title")
end

local function show_result(title, lines, paid)
  close_request()
  flow = nil
  outcome = {title = title, lines = lines, paid = paid}
  enter("result", cfg.result_timeout_s * 1000)
end

-- nil when a duel can be settled from this badge, else the reason it cannot.
local function not_ready()
  if not wallet.provisioned() then return "not_provisioned" end
  if not wallet.time_ok() then return "no_time" end
  return nil
end

-- ---------------------------------------------------------------------------------------------
-- Rounds
-- ---------------------------------------------------------------------------------------------

local function arm(number, delay)
  round = number
  go_at = now() + delay
  enter("armed", delay + cfg.reaction_max_ms + cfg.round_timeout_s * 1000)
  log(string.format("go %d %d", round, delay))
end

local function send_go()
  local left = go_at - now()
  if left > 0 then send(T_GO, game .. string.char(round) .. u16(left), opp.mac) end
end

-- Inviter only: chooses the delay and starts the next round on both badges.
local function start_round()
  arm(round + 1, random_between(cfg.delay_min_ms, math.min(cfg.delay_max_ms, 60000)))
  send_go()
  next_at = now() + cfg.resend_ms
end

local function send_time(number)
  send(T_TIME, game .. string.char(number) .. u16(mine[number]), opp.mac)
end

local function score_round()
  local a, b = mine[round], theirs[round]
  local verdict = "tie"
  if a < b then
    wins_me = wins_me + 1
    verdict = "won"
  elseif b < a then
    wins_them = wins_them + 1
    verdict = "lost"
  end
  last = {mine = a, theirs = b, verdict = verdict}
  log(string.format("round %d %d %d %s", round, a, b, verdict))
  enter("round_result", cfg.round_pause_ms + cfg.round_timeout_s * 1000)
  pause_until = now() + cfg.round_pause_ms
end

local function set_time(ms)
  mine[round] = ms
  log(string.format("time %d %d", round, ms))
  send_time(round)
  if theirs[round] then
    score_round()
  else
    state = "wait_time"                       -- the round's deadline stays
    next_at = now() + cfg.resend_ms
  end
end

-- After a round's result has been shown: the duel is decided, drawn, or goes on.
local function after_round()
  local need = cfg.rounds // 2 + 1
  local score = wins_me .. "-" .. wins_them
  if wins_me >= need then
    log("over won " .. score)
    enter("request", cfg.settle_timeout_s * 1000)
  elseif wins_them >= need then
    log("over lost " .. score)
    enter("find", cfg.settle_timeout_s * 1000)
  elseif round >= cfg.max_rounds then
    log("over draw " .. score)
    show_result("DRAW", {"Nobody won " .. need .. " rounds.", "No stake changes hands."})
  elseif role == "inviter" then
    start_round()
  end                                         -- the guest waits for GO; the state's deadline runs
end

-- ---------------------------------------------------------------------------------------------
-- Settling: the winner asks, the loser pays
-- ---------------------------------------------------------------------------------------------

local function unpaid(why, words)
  log("unpaid " .. why)
  show_result("UNPAID", {words, "The stake was not paid."})
end

local function open_request()
  local ttl = math.max(10, math.min(600, cfg.settle_timeout_s))
  local req, why = wallet.request_open{amount = stake, symbol = cfg.symbol, ttl_s = ttl}
  if not req then
    log("request err " .. tostring(why))
    unpaid(tostring(why), vk.reason_text(why, "request"))
    return
  end
  request = req
  watch = vk.receive.start{req_id = req.req_id, amount = stake, symbol = cfg.symbol,
                           every_ms = cfg.confirm_every_s * 1000}
  log("request " .. req.req_id)
  state = "await_pay"                         -- the settle deadline keeps running
end

-- A RESULT frame is unauthenticated: status 0 only queues its transaction for vk.receive, and
-- status 1 or 2 is a note, never the end (a stranger could send it). The settle deadline decides.
local function on_result(result)
  if (state ~= "await_pay" and state ~= "confirming") or not watch or result.req_id ~= request.req_id then
    return
  end
  log("result " .. result.status)
  if watch:result(result) then state = "confirming" end
end

local function confirm_payment()
  local status, detail = watch:update()       -- at most one blocking call
  if status == "paid" then
    local sig = vk.short(watch.sig)
    log("paid " .. sig)
    pulse("green", cfg.result_led_ms)
    show_result("PAID", {"Verified on chain.", sig}, true)
  elseif status == "failed" then
    log("check failed " .. tostring(detail))
    state = "await_pay"                       -- wait for another RESULT until the deadline
  end
end

local function find_request()
  if now() < next_at then return end
  next_at = now() + cfg.find_every_ms
  local want = norm(stake)
  for _, entry in ipairs(wallet.requests() or {}) do
    if entry.payee == opp.address and norm(entry.amount) == want
        and (entry.rail == nil or entry.rail == "solana")
        and (cfg.symbol == nil or entry.currency == cfg.symbol) then
      flow = vk.pay.start{request = entry}
      pay_state = nil
      log("paying " .. entry.req_id)
      enter("paying", cfg.pay_timeout_s * 1000)
      return
    end
  end
end

local function run_payment()
  local step, detail = flow:update()
  if step ~= pay_state then
    -- Each step gets its own time: the app is paused while the firmware approval is open.
    pay_state = step
    deadline = now() + cfg.pay_timeout_s * 1000
    log("pay " .. step)
  end
  if step == "done" then
    log("pay done " .. tostring(detail))
    pulse("green", cfg.result_led_ms)
    show_result("PAID", {"You paid the stake.", vk.short(detail)})
  elseif step == "failed" then
    local reason = flow:reason()
    log("pay failed " .. tostring(detail))
    if flow.request and not flow.result_sent then
      -- vk.pay tells the payee only about failures from the approval on: tell it of this one too.
      local frame = vk.result_frame(flow.request.req_id, vk.RESULT_FAILED)
      if frame then espnow.send(flow.request.mac, frame) end
    end
    show_result("NOT PAID", {"Not paid.", vk.reason_text(reason)})
  end
end

-- ---------------------------------------------------------------------------------------------
-- Frames
-- ---------------------------------------------------------------------------------------------

local function on_invite(mac, body)
  if state ~= "title" or #body < 41 or #body > 60 then return end
  local text = body:sub(41)
  local amount = norm(text)
  if not amount or amount == "0" then return end
  local address = codec.b58enc(body:sub(9, 40))
  if not address or address == wallet.address() then return end
  offer = {game = body:sub(1, 8), mac = mac, address = address, name = peer_name(mac), stake = text}
  enter("invited", cfg.offer_timeout_s * 1000)
  log("invited " .. text .. " " .. mac)
end

local function on_accept(mac, body)
  if state ~= "inviting" or #body ~= 40 or body:sub(1, 8) ~= game then return end
  local address = codec.b58enc(body:sub(9, 40))
  if not address or address == wallet.address() then return end
  opp = {mac = mac, address = address, name = peer_name(mac)}
  log("accepted " .. mac)
  start_round()
end

local function from_opponent(mac, body, length)
  return game ~= nil and opp ~= nil and mac == opp.mac and #body == length and body:sub(1, 8) == game
end

local function on_go(mac, body)
  if role ~= "guest" or not from_opponent(mac, body, 11) then return end
  if state ~= "starting" and state ~= "round_result" then return end
  if body:byte(9) ~= round + 1 then return end
  arm(round + 1, body:byte(10) * 256 + body:byte(11))
end

local function on_time(mac, body)
  if not from_opponent(mac, body, 11) then return end
  local number = body:byte(9)
  if number < 1 or number > round then return end
  if theirs[number] == nil then
    theirs[number] = body:byte(10) * 256 + body:byte(11)
    if number == round and state == "wait_time" then score_round() end
  elseif mine[number] then
    send_time(number)                         -- it is still asking: our TIME was lost
  end
end

local function on_leave(mac, body)
  if not from_opponent(mac, body, 8) then return end
  if PLAYING[state] then
    log("opponent left")
    show_result("NO DUEL", {"The other badge left.", "No stake changes hands."})
  elseif state == "request" or state == "await_pay" then
    unpaid("left", "The loser left without paying.")
  end
end

function on_espnow(mac, data, rssi)
  local body = vk.app_body(data, T_INVITE)
  if body then return on_invite(mac, body) end
  body = vk.app_body(data, T_ACCEPT)
  if body then return on_accept(mac, body) end
  body = vk.app_body(data, T_GO)
  if body then return on_go(mac, body) end
  body = vk.app_body(data, T_TIME)
  if body then return on_time(mac, body) end
  body = vk.app_body(data, T_LEAVE)
  if body then return on_leave(mac, body) end
  local result = vk.result_parse(data)
  if result then return on_result(result) end
end

-- ---------------------------------------------------------------------------------------------
-- Buttons
-- ---------------------------------------------------------------------------------------------

local function invite()
  local why = not_ready()
  if why then
    note = vk.reason_text(why)
    log("not ready " .. why)
    return
  end
  game = badge.system.random_bytes(8)
  role, stake = "inviter", cfg.stakes[stake_i]
  enter("inviting", cfg.invite_timeout_s * 1000)
  log("invite " .. codec.hex(game))
end

local function accept()
  local why = not_ready()
  if why then
    log("not ready " .. why)
    to_title(vk.reason_text(why))
    return
  end
  game, role, stake = offer.game, "guest", offer.stake
  opp = {mac = offer.mac, address = offer.address, name = offer.name}
  offer = nil
  enter("starting", cfg.start_timeout_s * 1000)
  log("accept " .. codec.hex(game))
end

-- CANCEL anywhere but the title: leave the duel.
local function leave()
  if flow and flow.sig and flow.request and not flow.result_sent then
    -- Already signed: give the winner the signature, so that it can look the payment up itself.
    local frame = vk.result_frame(flow.request.req_id, vk.RESULT_OK, flow.sig)
    if frame then espnow.send(flow.request.mac, frame) end
  elseif TELL[state] and game and opp then
    send(T_LEAVE, game, opp.mac)
  end
  log("left")
  to_title("")
end

function on_button(key, pressed)
  if not pressed then return end
  if key == "b" then
    if state == "title" then badge.system.exit() else leave() end
    return
  end
  if state == "title" then
    local step = key == "down" and 1 or key == "up" and -1 or 0
    if step ~= 0 then
      stake_i = (stake_i - 1 + step) % #cfg.stakes + 1
      note = ""
      log("stake " .. cfg.stakes[stake_i])
    elseif key == "a" then
      invite()
    end
  elseif key ~= "a" then
    return
  elseif state == "invited" then
    accept()
  elseif state == "armed" then
    set_time(FALSE_START)
  elseif state == "flash" then
    set_time(math.max(0, math.min(now() - go_at, cfg.reaction_max_ms - 1)))
  elseif state == "result" then
    to_title("")
  end
end

-- ---------------------------------------------------------------------------------------------
-- Frame loop
-- ---------------------------------------------------------------------------------------------

local function on_timeout()
  local was = state
  log(was == "inviting" and "invite timeout" or "timeout " .. was)
  if was == "inviting" then
    to_title("Nobody answered.")
  elseif was == "invited" or was == "result" then
    to_title("")
  elseif was == "request" or was == "await_pay" or was == "confirming" then
    unpaid("timeout", "No confirmed payment in time.")
  elseif was == "find" then
    show_result("NOT SETTLED", {"The winner's request was not heard."})
  elseif was == "paying" then
    show_result("NOT PAID", {"The payment did not finish in time."})
  else
    show_result("NO DUEL", {"The other badge stopped answering.", "No stake changes hands."})
  end
end

function on_start()
  if not espnow.enabled() then espnow.enable(true) end
  stake_i = math.max(1, math.min(#cfg.stakes, cfg.stake_default or 1))
  local tokens = wallet.tokens()
  symbol = cfg.symbol or (type(tokens) == "table" and tokens[1] and tokens[1].symbol) or ""
  log("up ch " .. tostring(espnow.channel()))
  log("title")
end

function on_update(dt)
  local t = now()
  if state == "inviting" then
    if t >= next_at then
      send(T_INVITE, game .. wallet.pubkey() .. stake)
      next_at = t + cfg.invite_every_ms
    end
  elseif state == "starting" then
    if t >= next_at then
      send(T_ACCEPT, game .. wallet.pubkey(), opp.mac)
      next_at = t + cfg.resend_ms
    end
  elseif state == "armed" then
    if t >= go_at then
      state = "flash"
      log("flash " .. round)
      pulse("led", cfg.flash_led_ms)
    elseif role == "inviter" and t >= next_at then
      send_go()
      next_at = t + cfg.resend_ms
    end
  elseif state == "flash" then
    if t - go_at >= cfg.reaction_max_ms then set_time(cfg.reaction_max_ms) end
  elseif state == "wait_time" then
    if t >= next_at then
      send_time(round)
      next_at = t + cfg.resend_ms
    end
  elseif state == "round_result" then
    if t >= pause_until then after_round() end
  elseif state == "request" then
    open_request()
  elseif state == "confirming" then
    confirm_payment()
  elseif state == "find" then
    find_request()
  elseif state == "paying" then
    run_payment()
  end
  if deadline and state ~= "title" and now() >= deadline then on_timeout() end
end

-- ---------------------------------------------------------------------------------------------
-- Screens: the stake on the stub, the state in the body
-- ---------------------------------------------------------------------------------------------

local function body_row(y, label, value, selected)
  ui.row(y, label, value, selected, nil, ui.BODY_X0, ui.BODY_X1)
  return y + ui.ROW_PITCH
end

local function body_lines(y, text, token)
  local color = ui.color(token or "ink")
  for _, line in ipairs(wrap(text, BODY_COLS)) do
    ui.text(line, ui.BODY_X0, y, color)
    y = y + LINE_PITCH
  end
  return y + 6
end

local function screen(title, hint, back)
  ui.page()
  ui.header(cfg.header)
  ui.perforation(ui.SPLIT_X, ui.CONTENT_Y, 212)
  ui.title(title, TITLE_Y, ui.BODY_CX)

  local amount = stake or (offer and offer.stake) or cfg.stakes[stake_i]
  if state == "result" and outcome.paid then
    ui.amount(ui.STUB_CX, 40, "", amount, symbol)
    ui.text_center("PAID", ui.STUB_CX, 40, ui.color("stamp_ok"))
  else
    ui.amount(ui.STUB_CX, 40, "STAKE", amount, symbol)
  end
  if round > 0 then
    ui.text_center(string.format("YOU %d \xC2\xB7 %d THEM", wins_me, wins_them), ui.STUB_CX, 124)
  end
  ui.footer(hint, back)
end

local function opponent_name(who)
  return who.name or vk.short(who.address)
end

function on_draw()
  local y = BODY_Y
  if state == "flash" then
    -- The flash: the whole screen in the ink colour.
    local text, size = "PRESS", 4
    gfx.clear(ui.color("ink"))
    gfx.text(text, (ui.W - #text * 6 * size) // 2, 92, ui.color("paper"), size)
    ui.text_center("SELECT now", ui.W // 2, 150, ui.color("paper"))
  elseif state == "title" then
    screen("DUEL", "UP/DOWN stake \xC2\xB7 SELECT invite", "CANCEL quit")
    y = body_row(y, "BEST OF", tostring(cfg.rounds)) + 4
    for i = 1, math.min(#cfg.stakes, 6) do
      y = body_row(y, cfg.stakes[i] .. " " .. symbol, i == stake_i and "STAKE" or "", i == stake_i)
    end
    if note ~= "" then body_lines(y + 2, note, "sub") end
  elseif state == "inviting" then
    screen("INVITING", "", "CANCEL back")
    y = body_lines(y, "Looking for another badge running Duel.")
    body_row(y, "TIME LEFT", seconds_left())
  elseif state == "invited" then
    screen("CHALLENGE", "SELECT accept", "CANCEL decline")
    y = body_lines(y, "Duel " .. opponent_name(offer) .. " for " .. offer.stake .. " " .. symbol .. "?")
    y = body_row(y, "KEY", vk.short(offer.address))
    y = body_row(y, "BEST OF", tostring(cfg.rounds))
    body_lines(y, "The name is self-named, not verified. The loser pays the winner.", "sub")
  elseif state == "starting" then
    screen("GET READY", "", "CANCEL leave")
    y = body_lines(y, "Accepted. Waiting for the first round.")
    body_row(y, "VS", opponent_name(opp))
  elseif state == "armed" then
    screen("ROUND " .. round, "SELECT at the flash", "CANCEL leave")
    y = body_lines(y, "Wait for the flash, then press SELECT. Too early loses the round.")
    body_row(y, "VS", opponent_name(opp))
  elseif state == "wait_time" then
    screen("ROUND " .. round, "", "CANCEL leave")
    y = body_row(y, "YOU", time_text(mine[round]))
    body_lines(y + 4, "Waiting for the other badge.", "sub")
  elseif state == "round_result" then
    local words = {won = "ROUND WON", lost = "ROUND LOST", tie = "ROUND TIED"}
    screen(words[last.verdict], "", "CANCEL leave")
    y = body_row(y, "ROUND", tostring(round))
    y = body_row(y, "YOU", time_text(last.mine))
    y = body_row(y, "THEM", time_text(last.theirs))
    if last.verdict == "tie" then body_lines(y + 4, "A tied round is played again.", "sub") end
  elseif state == "request" or state == "await_pay" or state == "confirming" then
    local words = {request = "Opening the request.", await_pay = "Waiting for the payment.",
                   confirming = "Checking the payment on chain."}
    screen("YOU WON", "", "CANCEL leave")
    y = body_lines(y, words[state])
    y = body_row(y, "FROM", opponent_name(opp))
    body_row(y, "TIME LEFT", seconds_left())
  elseif state == "find" or state == "paying" then
    screen("YOU LOST", "", "CANCEL leave")
    local words = "Looking for the winner's request."
    if state == "paying" then words = PAY_WORDS[flow.state] or "Paying." end
    y = body_lines(y, words)
    body_row(y, "TO", opponent_name(opp))
  else
    screen(outcome.title, "SELECT again", "CANCEL back")
    for i = 1, #outcome.lines do
      y = body_lines(y, outcome.lines[i], i == 1 and "ink" or "sub") - 2
    end
  end
end

-- Draw only when the screen changed (lib/vk.lua, "ui.frame"): a frame every pass would hold the
-- loop at 20 passes a second.
local draw_frame = on_draw
local drawn_state
function on_draw()
  if state ~= drawn_state then           -- the flash must reach the screen on the pass it begins
    drawn_state = state
    ui.dirty()
  end
  ui.frame(draw_frame)
end
