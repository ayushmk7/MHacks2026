// Dev tools: the serial commands that let a script see the screen and press buttons over USB
// (testing.md, "Dev hooks"). They defeat the physical-press guarantee, so the whole file is
// compiled only in the dev profile; in the release profile it is empty.
#include "../../vk_build.h"

#if VK_TEST_HOOKS

#include <Arduino.h>

#include "devtools.h"

#include "../../../config.h"
#include "../../../hal/display.h"
#include "../../../lua_sdk/lua_runtime.h"
#include "../../../settings.h"
#include "../../core/clock.h"
#include "../../core/config.h"
#include "../../core/serial.h"
#include "../../host/native.h"
#include "../../host/notify.h"
#include "../../shell/screens.h"   // ::shell::screenName()
#include "../../vk.h"              // vk::flushStats()
#include "../../wallet/reason.h"
#include "../../wallet/signer.h"
#include "../../wallet/approval.h"  // last: it removes the Arduino core's DISABLED macro

namespace {

// ---------------------------------------------------------------------------
// Button injection (hook H17)
// ---------------------------------------------------------------------------

constexpr uint8_t QUEUE_MAX = 16;
constexpr uint32_t TAP_MS = 80;
constexpr uint32_t HOLD_MAX_MS = 60000;

struct InjectedEvent {
  uint8_t key;
  bool press;      // false = release
  bool armed;      // false: a release still waiting for its own press to be applied
  uint32_t at;     // millis() at which the event is due; valid once armed
  uint32_t delay;  // release only: how long after its press began
};

InjectedEvent sQueue[QUEUE_MAX];
uint8_t sQueued = 0;
uint8_t sHeld = 0;  // keys the injector is holding down

bool enqueue(uint8_t key, bool press, bool armed, uint32_t at, uint32_t delay) {
  if (sQueued >= QUEUE_MAX) return false;
  sQueue[sQueued++] = InjectedEvent{key, press, armed, at, delay};
  return true;
}

// A press now and its release `ms` after the pass on which the press is applied, so the key is
// seen down for the full time however long the loop took to get to it.
bool enqueuePressFor(uint8_t key, uint32_t ms) {
  if (sQueued + 2 > QUEUE_MAX) return false;
  enqueue(key, true, true, millis(), 0);
  enqueue(key, false, false, 0, ms);
  return true;
}

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------

String nextToken(String &rest) {
  rest.trim();
  const int space = rest.indexOf(' ');
  if (space < 0) {
    const String token = rest;
    rest = "";
    return token;
  }
  const String token = rest.substring(0, space);
  rest = rest.substring(space + 1);
  return token;
}

bool parseU32(const String &text, uint32_t &out) {
  const size_t length = text.length();
  if (length == 0 || length > 10) return false;
  uint64_t value = 0;
  for (size_t i = 0; i < length; ++i) {
    const char c = text[i];
    if (c < '0' || c > '9') return false;
    value = value * 10 + (uint64_t)(c - '0');
  }
  if (value > 0xFFFFFFFFULL) return false;
  out = (uint32_t)value;
  return true;
}

int keyFromName(String name) {
  name.toLowerCase();
  if (name == "up") return BTN_UP;
  if (name == "down") return BTN_DOWN;
  if (name == "left") return BTN_LEFT;
  if (name == "right") return BTN_RIGHT;
  if (name == "a") return BTN_A;
  if (name == "b") return BTN_B;
  return -1;
}

// Appends a JSON string. `text` is read up to `max` bytes or its NUL. Bytes outside printable
// ASCII become '?', which is also what the approval screen draws for them.
void jsonString(String &out, const char *text, size_t max) {
  out += '"';
  for (size_t i = 0; text != nullptr && i < max && text[i] != '\0'; ++i) {
    const uint8_t c = (uint8_t)text[i];
    if (c == '"' || c == '\\') {
      out += '\\';
      out += (char)c;
    } else if (c < 0x20 || c > 0x7E) {
      out += '?';
    } else {
      out += (char)c;
    }
  }
  out += '"';
}

void jsonString(String &out, const char *text) { jsonString(out, text, (size_t)-1); }

const char *phaseName(vk::wallet::approval::Phase phase) {
  using vk::wallet::approval::Phase;
  switch (phase) {
    case Phase::IDLE:         return "IDLE";
    case Phase::WAIT_RELEASE: return "WAIT_RELEASE";
    case Phase::ARMED:        return "ARMED";
    case Phase::HOLDING:      return "HOLDING";
    case Phase::SIGNING:      return "SIGNING";
    case Phase::RESULT:       return "RESULT";
  }
  return "?";
}

const char *severityName(vk::wallet::Severity severity) {
  using vk::wallet::Severity;
  switch (severity) {
    case Severity::GREEN: return "green";
    case Severity::AMBER: return "amber";
    case Severity::RED:   return "red";
  }
  return "?";
}

// The rule in force, not the field as the decoder wrote it: a red request is closed unless the
// engine set dev_override, in which case it is a hold (approval.md, "Dev builds").
const char *selectName(const vk::wallet::ApprovalRequest &request) {
  using vk::wallet::SelectRule;
  SelectRule rule = request.select;
  if (request.severity == vk::wallet::Severity::RED) {
    rule = request.dev_override ? SelectRule::HOLD : SelectRule::DISABLED;
  }
  switch (rule) {
    case SelectRule::PRESS:    return "press";
    case SelectRule::HOLD:     return "hold";
    case SelectRule::DISABLED: return "disabled";
  }
  return "?";
}

const char *pollName(vk::wallet::Poll poll) {
  using vk::wallet::Poll;
  switch (poll) {
    case Poll::IDLE:    return "idle";
    case Poll::PENDING: return "pending";
    case Poll::SIGNED:  return "signed";
    case Poll::FAILED:  return "failed";
  }
  return "?";
}

const char *timeName(vk::clock::Source source) {
  using vk::clock::Source;
  switch (source) {
    case Source::NONE:  return "none";
    case Source::FLOOR: return "floor";
    case Source::SNTP:  return "sntp";
  }
  return "?";
}

// CRC-32 (IEEE, the one zlib computes), four bits at a time.
const uint32_t CRC_NIBBLE[16] = {
    0x00000000, 0x1DB71064, 0x3B6E20C8, 0x26D930AC, 0x76DC4190, 0x6B6B51F4, 0x4DB26158, 0x5005713C,
    0xEDB88320, 0xF00F9344, 0xD6D6A3E8, 0xCB61B38C, 0x9B64C2B0, 0x86D3D2D4, 0xA00AE278, 0xBDBDF21C};

inline uint32_t crcByte(uint32_t crc, uint8_t byte) {
  crc ^= byte;
  crc = (crc >> 4) ^ CRC_NIBBLE[crc & 0x0F];
  crc = (crc >> 4) ^ CRC_NIBBLE[crc & 0x0F];
  return crc;
}

// Standard base64 with padding. `out` needs 4 * ceil(length / 3) + 1 bytes.
void base64Encode(const uint8_t *data, size_t length, char *out) {
  static const char ALPHABET[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  size_t o = 0;
  for (size_t i = 0; i < length; i += 3) {
    const size_t left = length - i;
    const uint32_t chunk = ((uint32_t)data[i] << 16) | (left > 1 ? (uint32_t)data[i + 1] << 8 : 0) |
                           (left > 2 ? (uint32_t)data[i + 2] : 0);
    out[o++] = ALPHABET[(chunk >> 18) & 0x3F];
    out[o++] = ALPHABET[(chunk >> 12) & 0x3F];
    out[o++] = left > 1 ? ALPHABET[(chunk >> 6) & 0x3F] : '=';
    out[o++] = left > 2 ? ALPHABET[chunk & 0x3F] : '=';
  }
  out[o] = '\0';
}

// ---------------------------------------------------------------------------
// Commands
// ---------------------------------------------------------------------------

void cmdState(const String &, const vk::serial::Reply &reply) {
  using namespace vk::wallet;
  const ApprovalRequest *request = approval::current();

  String out;
  out.reserve(768);
  out += "OK {\"app\":";
  jsonString(out, runtime::currentApp().c_str());
  out += ",\"native\":";
  out += vk::host::native::active() ? "true" : "false";
  out += ",\"screen\":\"";
  out += ::shell::screenName();            // [a-z_] or empty: nothing to escape
  out += "\"";
  out += ",\"modal\":";
  out += approval::active() ? "true" : "false";
  out += ",\"phase\":";
  jsonString(out, phaseName(approval::phase()));
  out += ",\"severity\":";
  jsonString(out, request ? severityName(request->severity) : "");
  out += ",\"select\":";
  jsonString(out, request ? selectName(*request) : "");
  out += ",\"title\":";
  jsonString(out, request ? request->title : "", sizeof request->title);
  out += ",\"headline\":";
  jsonString(out, request ? request->headline : "", sizeof request->headline);
  out += ",\"big\":";
  jsonString(out, request ? request->big : "", sizeof request->big);
  out += ",\"sub\":";
  jsonString(out, request ? request->sub : "", sizeof request->sub);
  out += ",\"lines\":[";
  if (request) {
    const size_t maxLines = sizeof request->lines / sizeof request->lines[0];
    const size_t count = request->line_count < maxLines ? request->line_count : maxLines;
    for (size_t i = 0; i < count; ++i) {
      if (i) out += ',';
      out += '[';
      jsonString(out, request->lines[i].label, sizeof request->lines[i].label);
      out += ',';
      jsonString(out, request->lines[i].value, sizeof request->lines[i].value);
      out += ']';
    }
  }
  out += "],\"red_reason\":";
  jsonString(out, reasonName(request ? request->red_reason : VK_OK));
  out += ",\"dev_override\":";
  out += (request && request->dev_override) ? "true" : "false";
  out += ",\"provisioned\":";
  out += vk::config::provisioned() ? "true" : "false";
  out += ",\"time\":";
  jsonString(out, timeName(vk::clock::source()));
  out += ",\"notes\":";
  out += String((unsigned)vk::host::notify::count());
  out += ",\"poll\":";
  jsonString(out, pollName(approval::peekResult()));
  out += ",\"heap\":";
  out += String((unsigned)ESP.getFreeHeap());
  // What VKSHOT cannot see: it reads the canvas, not the glass. A backlight at 0, or a canvas that
  // is drawn but never sent to the panel, is a black screen with a perfect screenshot.
  out += ",\"backlight\":";
  out += String((unsigned)display::brightness());
  out += ",\"flushes\":";
  out += String((unsigned)vk::flushStats().transfers);   // canvas transfers since boot (hook H24)
  // The launcher (shell.md, "Launcher"): its open folder, its cursor and its cells, in grid order.
  // Kept while an app runs: they are what the launcher shows when it comes back.
  out += ",\"folder\":";
  jsonString(out, vk::shell::launcherFolder());
  out += ",\"cursor\":";
  out += String(vk::shell::launcherCursor());
  out += ",\"menu\":[";
  const int rows = vk::shell::launcherRowCount();
  for (int i = 0; i < rows; ++i) {
    if (i) out += ',';
    jsonString(out, vk::shell::launcherRowKey(i).c_str());
  }
  out += "]}";
  reply(out);
}

void cmdBtn(const String &args, const vk::serial::Reply &reply) {
  String rest = args;
  const String keyName = nextToken(rest);
  String action = nextToken(rest);
  const String msText = nextToken(rest);
  action.toLowerCase();

  const int key = keyFromName(keyName);
  uint32_t ms = 0;
  const bool hasMs = msText.length() > 0;
  if (key < 0 || (hasMs && (!parseU32(msText, ms) || ms > HOLD_MAX_MS))) {
    reply("ERR usage");
    return;
  }

  bool queued;
  if (action == "tap") {
    queued = enqueuePressFor((uint8_t)key, hasMs ? ms : TAP_MS);
  } else if (action == "hold") {
    if (!hasMs) {
      reply("ERR usage");
      return;
    }
    queued = enqueuePressFor((uint8_t)key, ms);
  } else if (action == "press") {
    queued = enqueue((uint8_t)key, true, true, millis(), 0);
  } else if (action == "release") {
    queued = enqueue((uint8_t)key, false, true, millis(), 0);
  } else {
    reply("ERR usage");
    return;
  }
  reply(queued ? "OK" : "ERR busy");
}

// The framebuffer as true RGB565, little-endian. A 16-bit LovyanGFX sprite keeps its pixels
// byte-swapped in memory, so the pixels are read with readPixel(), which returns the plain value.
// Runs of equal pixels become (count u8, pixel u16 little-endian) triples; 24 triples make one
// 96-character base64 line. The CRC-32 is of the 2 * width * height decoded bytes.
void cmdShot(const String &, const vk::serial::Reply &reply) {
  LGFX_Sprite &canvas = display::canvas();
  const int width = canvas.width();
  const int height = canvas.height();
  if (canvas.getBuffer() == nullptr || width <= 0 || height <= 0) {
    reply("ERR no_canvas");
    return;
  }
  reply("OK shot " + String(width) + " " + String(height));

  uint8_t raw[72];
  size_t rawLength = 0;
  char line[2 + 96 + 1] = "+ ";
  uint32_t crc = 0xFFFFFFFFUL;
  uint16_t runPixel = 0;
  uint16_t runLength = 0;

  auto flushLine = [&]() {
    base64Encode(raw, rawLength, line + 2);
    reply(String(line));
    rawLength = 0;
  };
  auto emitRun = [&]() {
    raw[rawLength++] = (uint8_t)runLength;
    raw[rawLength++] = (uint8_t)(runPixel & 0xFF);
    raw[rawLength++] = (uint8_t)(runPixel >> 8);
    if (rawLength == sizeof raw) flushLine();
  };

  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      const uint16_t pixel = canvas.readPixel(x, y);
      crc = crcByte(crc, (uint8_t)(pixel & 0xFF));
      crc = crcByte(crc, (uint8_t)(pixel >> 8));
      if (runLength > 0 && pixel == runPixel && runLength < 255) {
        ++runLength;
      } else {
        if (runLength > 0) emitRun();
        runPixel = pixel;
        runLength = 1;
      }
    }
  }
  if (runLength > 0) emitRun();
  if (rawLength > 0) flushLine();

  char tail[24];
  snprintf(tail, sizeof tail, "OK end %08lx", (unsigned long)(crc ^ 0xFFFFFFFFUL));
  reply(String(tail));
}

void cmdTime(const String &args, const vk::serial::Reply &reply) {
  String rest = args;
  uint32_t unixSeconds = 0;
  if (!parseU32(nextToken(rest), unixSeconds)) {
    reply("ERR usage");
    return;
  }
  vk::clock::devSet(unixSeconds);
  reply("OK");
}

void cmdPair(const String &, const vk::serial::Reply &reply) {
  reply("OK " + settings::pairingCode());
}

// VKNOTE <title>|<body>|<app>
void cmdNote(const String &args, const vk::serial::Reply &reply) {
  String title = args;
  String body;
  String appId;
  const int firstBar = args.indexOf('|');
  if (firstBar >= 0) {
    title = args.substring(0, firstBar);
    body = args.substring(firstBar + 1);
    const int secondBar = body.indexOf('|');
    if (secondBar >= 0) {
      appId = body.substring(secondBar + 1);
      body = body.substring(0, secondBar);
    }
  }
  title.trim();
  body.trim();
  appId.trim();
  if (title.length() == 0) {
    reply("ERR usage");
    return;
  }
  vk::host::notify::post(title.c_str(), body.c_str(), appId.c_str());
  reply("OK");
}

// ---------------------------------------------------------------------------
// VKPERF: main-loop timing (testing.md, "Dev hooks")
// ---------------------------------------------------------------------------
// perfTick() runs once per loop pass (from the H17 call at the top of buttons::update()), so the
// time between two calls is one whole pass: input, radios, services, the shell or the app, and the
// display transfer. Each window is one second; VKPERF prints the last complete one.

struct PerfWindow {
  uint32_t passes = 0;
  uint32_t sumUs = 0;
  uint32_t worstUs = 0;
  uint32_t slow = 0;       // passes of 20 ms or more: a pass that drew and sent the canvas
  uint32_t flushes = 0;    // canvas transfers to the panel (vk::flush, hook H24)
  uint32_t flushUs = 0;    // time spent in them
};

constexpr uint32_t PERF_SLOW_US = 20000;

PerfWindow sPerfNow;
PerfWindow sPerfLast;
vk::FlushStats sPerfFlushAt = {0, 0};   // vk::flushStats() when sPerfNow began
uint32_t sPerfPassAt = 0;      // micros() at the last pass; 0 = none yet
uint32_t sPerfWindowAt = 0;    // millis() when sPerfNow began
uint32_t sPerfPeakUs = 0;      // the longest pass since the last VKPERF

void perfTick() {
  const uint32_t nowUs = micros();
  if (sPerfPassAt != 0) {
    const uint32_t passUs = nowUs - sPerfPassAt;
    ++sPerfNow.passes;
    sPerfNow.sumUs += passUs;
    if (passUs > sPerfNow.worstUs) sPerfNow.worstUs = passUs;
    if (passUs >= PERF_SLOW_US) ++sPerfNow.slow;
    if (passUs > sPerfPeakUs) sPerfPeakUs = passUs;
  }
  sPerfPassAt = nowUs == 0 ? 1 : nowUs;
  const uint32_t nowMs = millis();
  if (nowMs - sPerfWindowAt >= 1000) {
    const vk::FlushStats flush = vk::flushStats();
    sPerfNow.flushes = flush.transfers - sPerfFlushAt.transfers;
    sPerfNow.flushUs = flush.micros - sPerfFlushAt.micros;
    sPerfFlushAt = flush;
    sPerfLast = sPerfNow;
    sPerfNow = PerfWindow();
    sPerfWindowAt = nowMs;
  }
}

void cmdPerf(const String &, const vk::serial::Reply &reply) {
  const PerfWindow &w = sPerfLast;
  char line[200];
  const uint32_t avgUs = w.passes ? w.sumUs / w.passes : 0;
  snprintf(line, sizeof line, "OK passes=%u avg_ms=%u.%u worst_ms=%u slow=%u flushes=%u flush_ms=%u peak_ms=%u stack=%u",
           (unsigned)w.passes, (unsigned)(avgUs / 1000), (unsigned)((avgUs % 1000) / 100),
           (unsigned)((w.worstUs + 500) / 1000), (unsigned)w.slow, (unsigned)w.flushes,
           (unsigned)(w.flushes ? (w.flushUs / w.flushes + 500) / 1000 : 0),
           (unsigned)((sPerfPeakUs + 500) / 1000),
           (unsigned)uxTaskGetStackHighWaterMark(NULL));   // loop task: least free stack since boot, bytes (M6)
  sPerfPeakUs = 0;
  reply(line);
}

}  // namespace

VK_SERIAL_COMMAND(vkperf, "VKPERF", cmdPerf, "loop passes in the last second: count, average and worst ms, display transfers");
VK_SERIAL_COMMAND(vkstate, "VKSTATE", cmdState, "one-line JSON snapshot of the badge");
VK_SERIAL_COMMAND(vkbtn, "VKBTN", cmdBtn, "<up|down|left|right|a|b> <tap|press|release|hold> [ms]");
VK_SERIAL_COMMAND(vkshot, "VKSHOT", cmdShot, "the screen: run-length RGB565 in base64, then a CRC-32");
VK_SERIAL_COMMAND(vktime, "VKTIME", cmdTime, "<unix> sets the clock and marks it synced");
VK_SERIAL_COMMAND(vkpair, "VKPAIR", cmdPair, "the push pairing code");
VK_SERIAL_COMMAND(vknote, "VKNOTE", cmdNote, "<title>|<body>|<app> posts a notification");

// Upstream has just cleared *pressed and *released for this pass and may have rewritten *down from
// the hardware. For every key the injector holds: force it on in *down, keep it out of *released,
// and set it in *pressed only on the pass its press begins. On the pass a release is applied the
// key leaves *down and is set in *released once.
extern "C" void vk_dev_apply_injected_buttons(uint8_t *down, uint8_t *pressed, uint8_t *released) {
  perfTick();
  const uint32_t now = millis();
  uint8_t began = 0;
  uint8_t ended = 0;
  uint8_t settled = 0;  // keys with an event applied or still waiting: at most one change per key per pass, in order

  uint8_t kept = 0;
  for (uint8_t i = 0; i < sQueued; ++i) {
    const InjectedEvent event = sQueue[i];
    const uint8_t bit = (uint8_t)(1U << event.key);
    const bool due = event.armed && (int32_t)(now - event.at) >= 0;
    if (!due || (settled & bit)) {
      settled |= bit;
      sQueue[kept++] = event;
      continue;
    }
    settled |= bit;
    if (event.press) {
      if (!(sHeld & bit)) {
        sHeld |= bit;
        began |= bit;
      }
      // Start the clock of the release that belongs to this press (a tap or a hold).
      for (uint8_t j = (uint8_t)(i + 1); j < sQueued; ++j) {
        InjectedEvent &later = sQueue[j];
        if (later.key != event.key) continue;
        if (!later.press && !later.armed) {
          later.armed = true;
          later.at = now + later.delay;
        }
        break;
      }
    } else if (sHeld & bit) {
      sHeld &= (uint8_t)~bit;
      ended |= bit;
    }
  }
  sQueued = kept;

  *pressed = (uint8_t)((*pressed & ~sHeld) | began);
  *down = (uint8_t)((*down | sHeld) & ~ended);
  *released = (uint8_t)((*released & ~sHeld) | ended);
}

#endif  // VK_TEST_HOOKS
