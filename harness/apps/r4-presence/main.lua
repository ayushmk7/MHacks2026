-- R4 presence
--
-- Dev-only (docs/specs/P1-r-test-harness.md, R4). The same app runs on both badges. Each badge is
-- always a payee: it answers every CHAL it receives with a signed PROOF. SELECT on either badge
-- makes it the payer for one run:
--
--   config.runs x { CHAL (60 B, 00 §5) unicast -> peer signs with wallet.sign_proof -> PROOF (76 B) }
--
-- timed with badge.millis() from creating the nonce to PROOF receipt, which is what
-- wallet.check_proof will time. A PROOF that does not arrive within config.lost_ms counts as lost.
-- The run's p50 / p95 / max and loss go on screen and in the log, are saved to storage, and are
-- POSTed to the laptop (harness/r4_timing.py serve) whenever Wi-Fi is up.
--
-- Signing uses the wallet's sign_proof: A's, or the dev stand-in in the temp_firmware fork
-- (src/lua_sdk/lib_wallet.cpp), which signs with the real identity key (SE050 or NVS). BadgeOS has
-- no sign_proof in Lua (presence runs inside its firmware), so this app needs the fork.
-- harness/r4_timing.py writes the laptop's address into config.lua and adds lib/vk.lua as vk.lua.
--
-- Log lines (each "[app] R4 ..."):
--   R4 start key=<loc> wifi=<on|off> ch=<n> signer=<stub|wallet|none>
--   R4 <status line>                      whenever it changes
--   R4 RUN wifi=... ch=... peer=<name>(<loc>) n=... p50=... p95=... max=... lost=<n>/<runs> late=... bad=... verify_max=...
--   R4 PAYEE answered=<n> sign p50=... p95=... max=... key=<loc>     every 50 PROOFs signed
--
-- SELECT run   DOWN upload saved runs   UP clear saved runs   CANCEL exit

local cfg = require("config")
local vk = require("vk")
local sys, now_ms = badge.system, badge.millis
local espnow, wifi, http, storage = badge.espnow, badge.wifi, badge.http, badge.storage
local wallet = badge.wallet

-- ── frames (00 §5, little-endian, header "VK" v1 type) ────────────────────────────────────────
local T_CHAL, T_PROOF = 2, 3
local HELLO = "R4H1"  -- harness only: pubkey[32] · key location[1] · name. Not a VK frame

local function header(t) return "VK\1" .. string.char(t) end
local function hex(bytes)
  local out = {}
  for i = 1, #bytes do out[i] = string.format("%02x", bytes:byte(i)) end
  return table.concat(out)
end

-- ── state ─────────────────────────────────────────────────────────────────────────────────────
local me = { pubkey = wallet and wallet.pubkey and wallet.pubkey(),
             loc = wallet and wallet.key_location and wallet.key_location() or "?" }
if me.pubkey == "" then me.pubkey = nil end   -- BadgeOS: no identity
local peers = {}          -- mac -> {pubkey, loc, name, rssi, seen}
local payee = { n = 0, sign = {}, fail = 0 }   -- CHALs answered here, with sign times (ms)
local session = nil       -- id of this launch's payee record; the laptop keeps its latest upload
local run = nil           -- the payer run in progress, or the last finished one
local pending = nil       -- {req_id, nonce, t0}
local timed_out = {}      -- req_id -> t0, for PROOFs that arrive after lost_ms
local next_at, last_hello = 0, 0
local status_line, status_tone = "SELECT: run " .. cfg.runs .. " exchanges", "mut"
local saved = 0

local function say(text, tone)
  status_line, status_tone = text, tone or "mut"
  badge.log("R4 " .. text)
  vk.ui.dirty()
end

local function signer()
  return wallet and (wallet.stub and "stub" or "wallet") or "none"
end

-- Nearest-rank percentile on a sorted array; the laptop uses the same rule.
local function pct(sorted, q)
  if #sorted == 0 then return nil end
  return sorted[math.max(1, math.ceil(q * #sorted))]
end

local function summarize(list)
  local s = {}
  for i, v in ipairs(list) do s[i] = v end
  table.sort(s)
  return { n = #s, p50 = pct(s, 0.50), p95 = pct(s, 0.95), max = s[#s] }
end

-- Every exchange of a run: received PROOFs, late ones at their real time, and math.huge for the
-- ones that never came, so a slow tail shows in p95 instead of hiding in the loss count.
local function all_times(r)
  local t = {}
  for _, v in ipairs(r.rtt) do t[#t + 1] = v end
  for _, v in ipairs(r.late) do t[#t + 1] = v end
  for _ = 1, r.lost - #r.late do t[#t + 1] = math.huge end
  return t
end

local function wifi_state()
  return wifi.connected() and "on" or "off"
end

-- ESP-NOW needs the Wi-Fi driver. Settings > Wi-Fi > Disconnect turns the radio fully off under
-- it, so bring ESP-NOW back up (on the Settings > ESP-NOW channel) whenever Wi-Fi is down.
local function rearm_espnow()
  if not wifi.connected() then espnow.enable(false) end
  if not espnow.enabled() then espnow.enable(true) end
end

local function best_peer()
  local best, t = nil, now_ms()
  for mac, p in pairs(peers) do
    if t - p.seen < 3 * cfg.hello_ms and (not best or p.rssi > best.rssi) then
      best = p
      best.mac = mac
    end
  end
  return best
end

-- ── results: saved as JSON lines, POSTed to the laptop when Wi-Fi is up ──────────────────────
-- Names keep only the characters they have always been uploaded with.
local function clean(s) return (tostring(s or ""):gsub("[^%w%._:%- ]", "")) end

local function run_json(r)
  local json = vk.json
  return json.encode({
    kind = "run", id = r.id, badge = clean(r.badge), badge_loc = r.badge_loc, peer = clean(r.peer),
    peer_name = clean(r.peer_name), peer_loc = r.peer_loc, wifi = r.wifi, ssid = clean(r.ssid),
    channel = r.channel, rssi = r.rssi, runs = cfg.runs, lost_ms = cfg.lost_ms, gap_ms = cfg.gap_ms,
    signer = r.signer, rtt_ms = json.array(r.rtt), lost = r.lost, send_fail = r.send_fail,
    late_ms = json.array(r.late), bad_sig = r.bad_sig, verify_ms = json.array(r.verify),
  })
end

local function payee_json()
  return vk.json.encode({
    kind = "payee", id = session, badge = clean(wallet.address and wallet.address() or ""), badge_loc = me.loc,
    wifi = wifi_state(), answered = payee.n, sign_fail = payee.fail, sign_ms = vk.json.array(payee.sign),
  })
end

local function count_saved()
  local text = storage.exists(cfg.results) and storage.read(cfg.results) or ""
  local n = 0
  for _ in text:gmatch("[^\n]+") do n = n + 1 end
  saved = n
end

local function upload()
  if not wifi.connected() then return say("Wi-Fi off: saved, upload later with DOWN", "warn") end
  local text = storage.exists(cfg.results) and storage.read(cfg.results) or ""
  if payee.n > 0 then text = text .. payee_json() .. "\n" end
  if text == "" then return say("nothing to upload", "mut") end
  local status, body = http.post(cfg.listener .. "/r4/result", text, "application/x-ndjson", cfg.upload_timeout_ms)
  if status == 200 then
    say("uploaded " .. saved .. " run(s) to laptop", "ok")
  else
    say("upload failed: " .. tostring(status or body), "bad")
  end
end

-- ── payer ────────────────────────────────────────────────────────────────────────────────────
local function ms_text(v) return v == math.huge and "lost" or tostring(v) end

local function finish()
  local s = summarize(all_times(run))
  run.done, run.stats = true, s
  storage.append(cfg.results, run_json(run) .. "\n")
  count_saved()
  badge.log(string.format("R4 RUN wifi=%s ch=%d peer=%s(%s) n=%d p50=%s p95=%s max=%s lost=%d/%d late=%d bad=%d verify_max=%s",
            run.wifi, run.channel, run.peer_name, run.peer_loc, s.n, ms_text(s.p50), ms_text(s.p95),
            ms_text(s.max), run.lost, cfg.runs, #run.late, run.bad_sig, tostring(summarize(run.verify).max)))
  vk.ui.dirty()
  upload()
end

local function send_chal()
  local req_id = sys.random_bytes(8)
  local nonce
  if wallet.new_nonce and not wallet.stub then
    nonce = wallet.new_nonce(req_id)   -- A's firmware stores {req_id, nonce, t0} itself
  else
    nonce = sys.random_bytes(16)
  end
  local t0 = now_ms()
  local frame = header(T_CHAL) .. req_id .. nonce .. me.pubkey
  pending = { req_id = req_id, nonce = nonce, t0 = t0 }
  if not espnow.send(run.mac, frame) then
    run.send_fail, run.lost = run.send_fail + 1, run.lost + 1
    pending, next_at = nil, now_ms() + cfg.gap_ms
  end
end

local function start_run()
  if not me.pubkey then return say("no identity key on this badge", "bad") end
  rearm_espnow()
  local p = best_peer()
  if not p then return say("no peer heard: run this app on the other badge", "bad") end
  run = {
    id = hex(sys.random_bytes(6)), badge = wallet.address and wallet.address() or "",
    badge_loc = me.loc, peer = p.address, peer_name = p.name, peer_loc = p.loc, mac = p.mac,
    peer_pubkey = p.pubkey, wifi = wifi_state(), ssid = wifi.connected() and wifi.ssid() or "",
    channel = espnow.channel(), rssi = p.rssi, signer = signer(),
    rtt = {}, late = {}, verify = {}, lost = 0, send_fail = 0, bad_sig = 0, i = 0, done = false,
  }
  timed_out = {}
  say(string.format("run %s: %d x CHAL -> %s, Wi-Fi %s", run.id, cfg.runs, p.name, run.wifi))
  next_at = now_ms()
end

local function check(p, sig)
  if wallet.check_proof and not wallet.stub then
    return wallet.check_proof(p.req_id, run.peer_pubkey, sig) ~= "bad_sig"
  end
  return wallet.verify_proof(p.req_id, p.nonce, me.pubkey, run.peer_pubkey, sig)
end

local function on_proof(data)
  local req_id, sig = data:sub(5, 12), data:sub(13, 76)
  local t = now_ms()
  if pending and req_id == pending.req_id then
    local p = pending
    pending = nil
    run.rtt[#run.rtt + 1] = t - p.t0   -- recorded before verifying, as check_proof times receipt
    if not check(p, sig) then run.bad_sig = run.bad_sig + 1 end
    run.verify[#run.verify + 1] = now_ms() - t
    next_at = now_ms() + cfg.gap_ms
  elseif timed_out[req_id] then
    run.late[#run.late + 1] = t - timed_out[req_id]
    timed_out[req_id] = nil
  end
end

-- ── payee ────────────────────────────────────────────────────────────────────────────────────
local function on_chal(mac, data)
  local req_id, nonce, payer = data:sub(5, 12), data:sub(13, 28), data:sub(29, 60)
  local t0 = now_ms()
  local sig = wallet.sign_proof(req_id, nonce, payer)
  local ms = now_ms() - t0
  if not sig or #sig ~= 64 then
    payee.fail = payee.fail + 1
    return
  end
  espnow.send(mac, header(T_PROOF) .. req_id .. sig)
  payee.n = payee.n + 1
  payee.sign[#payee.sign + 1] = ms
  if #payee.sign > 1000 then table.remove(payee.sign, 1) end
  if payee.n % 50 == 0 then
    local s = summarize(payee.sign)
    badge.log(string.format("R4 PAYEE answered=%d sign p50=%d p95=%d max=%d key=%s",
              payee.n, s.p50, s.p95, s.max, me.loc))
    vk.ui.dirty()
  end
end

-- ── callbacks ────────────────────────────────────────────────────────────────────────────────
function on_start()
  session = hex(sys.random_bytes(6))
  rearm_espnow()
  count_saved()
  badge.log(string.format("R4 start key=%s wifi=%s ch=%d signer=%s", me.loc, wifi_state(), espnow.channel(), signer()))
  if not (wallet and wallet.sign_proof) then say("no wallet.sign_proof: flash the harness firmware", "bad") end
end

function on_espnow(mac, data, rssi)
  if data:sub(1, 4) == HELLO and #data >= 37 then
    local pubkey = data:sub(5, 36)
    local loc = ({ s = "se050", n = "nvs" })[data:sub(37, 37)] or "?"
    local p = peers[mac] or {}
    p.pubkey, p.loc, p.name, p.rssi, p.seen = pubkey, loc, data:sub(38), rssi, now_ms()
    p.address = p.address or hex(pubkey)
    peers[mac] = p
    return
  end
  if #data < 4 or data:sub(1, 3) ~= "VK\1" then return end
  local t = data:byte(4)
  if t == T_CHAL and #data == 60 and wallet and wallet.sign_proof then on_chal(mac, data)
  elseif t == T_PROOF and #data == 76 and run and not run.done then on_proof(data) end
end

function on_update(dt)
  local t = now_ms()

  if t - last_hello >= cfg.hello_ms and me.pubkey then
    last_hello = t
    local loc = me.loc == "se050" and "s" or (me.loc == "nvs" or me.loc == "software") and "n" or "?"
    espnow.broadcast(HELLO .. me.pubkey .. loc .. badge.device_name)
  end

  if not run or run.done then return end
  if pending and t - pending.t0 > cfg.lost_ms then
    timed_out[pending.req_id] = pending.t0
    run.lost, pending, next_at = run.lost + 1, nil, t + cfg.gap_ms
  end
  if not pending and t >= next_at then
    if run.i >= cfg.runs then
      -- give any straggling PROOF one more lost_ms before closing the run
      if t >= next_at + cfg.lost_ms then finish() end
      return
    end
    run.i = run.i + 1
    send_chal()
  end
end

function on_button(key, pressed)
  if not pressed then return end
  if key == "a" and not (run and not run.done) then start_run()
  elseif key == "down" then upload()
  elseif key == "up" then storage.remove(cfg.results); count_saved(); say("saved runs cleared")
  elseif key == "b" then sys.exit() end
end

local function stat_text(s)
  if not s or s.n == 0 then return "-" end
  return string.format("p50 %s  p95 %s  max %s", ms_text(s.p50), ms_text(s.p95), ms_text(s.max))
end

local function draw()
  local ui = vk.ui
  local tones = { ok = "stamp_ok", warn = "stamp_warn", bad = "stamp_bad", mut = "faint" }
  local y, p = ui.LIST_Y, ui.ROW_PITCH
  ui.page()
  ui.header(cfg.header)
  ui.title("R4 PRESENCE", ui.TITLE_Y)
  ui.row(y, "KEY / SIGNER", me.loc .. " / " .. signer())
  ui.row(y + p, "WI-FI / CHANNEL", wifi_state() .. " / " .. espnow.channel())
  local peer = best_peer()
  ui.row(y + 2 * p, "PEER", peer and string.format("%s %s %d dBm", peer.name, peer.loc, peer.rssi) or "none heard",
         false, not peer and ui.color("stamp_warn") or nil)
  if run then
    local s = run.done and run.stats or summarize(all_times(run))
    local lost_pct = run.i > 0 and 100 * run.lost / run.i or 0
    ui.row(y + 3 * p, "PAYER " .. (run.done and "DONE" or "RUNNING"),
           string.format("%d/%d lost %d (%.1f%%) late %d bad %d", run.i, cfg.runs, run.lost, lost_pct, #run.late, run.bad_sig),
           false, run.lost > 0 and ui.color("stamp_warn") or nil)
    ui.row(y + 4 * p, "ROUND TRIP", stat_text(s), false, run.done and ui.color("stamp_ok") or nil)
  else
    ui.row(y + 3 * p, "PAYER", "not run", false, ui.color("faint"))
  end
  ui.row(y + 5 * p, "PAYEE ANSWERED", string.format("%d  sign failures %d", payee.n, payee.fail))
  ui.row(y + 6 * p, "PAYEE SIGN", stat_text(summarize(payee.sign)))
  ui.row(y + 7 * p, "SAVED RUNS", tostring(saved))
  ui.rule(y + 8 * p - 4)
  ui.text(status_line, ui.MARGIN, y + 8 * p + 2, ui.color(tones[status_tone]))
  ui.footer("SELECT run  DOWN upload  UP clear", "CANCEL quit")
end

function on_draw()
  local running = run and not run.done
  vk.ui.frame(draw, running and cfg.redraw_ms or cfg.idle_ms)
end
