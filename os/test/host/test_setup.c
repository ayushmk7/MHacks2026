// LINK: src/vk/core/utc_offset.c src/vk/shell/setup_core.c
// Host test of the on-badge settings logic (docs/os/ui/shell.md, "Badge", "Setup", "Advanced"):
//   - the UTC offset: parsing, formatting, stepping, the list of choices, applying it to a time;
//   - the number stepper the settings pages use for U32 keys (a ladder inside the key's range);
//   - the setup checklist computed from a state struct, the steps left, the first-boot rule and
//     the "missing" line for the wallet row.
// Run: test/host/run.sh test_setup
#include <stdio.h>
#include <string.h>

#include "../../src/vk/core/utc_offset.h"
#include "../../src/vk/shell/setup_core.h"

static int failures = 0;

#define CHECK(cond, ...)                              \
  do {                                                \
    if (!(cond)) {                                    \
      ++failures;                                     \
      printf("FAIL %s:%d: ", __FILE__, __LINE__);     \
      printf(__VA_ARGS__);                            \
      printf("\n");                                   \
    }                                                 \
  } while (0)

// ---- UTC offset -----------------------------------------------------------------------------------

static void test_offset_parse(void) {
  int32_t m = 99;
  CHECK(vk_utc_offset_parse("", &m) && m == 0, "empty is UTC: %d", (int)m);
  CHECK(vk_utc_offset_parse("+00:00", &m) && m == 0, "+00:00");
  CHECK(vk_utc_offset_parse("-00:00", &m) && m == 0, "-00:00 is UTC too");
  CHECK(vk_utc_offset_parse("+05:30", &m) && m == 330, "+05:30 -> %d", (int)m);
  CHECK(vk_utc_offset_parse("-04:00", &m) && m == -240, "-04:00 -> %d", (int)m);
  CHECK(vk_utc_offset_parse("+05:45", &m) && m == 345, "+05:45 (Nepal) -> %d", (int)m);
  CHECK(vk_utc_offset_parse("-12:00", &m) && m == -720, "the lowest offset");
  CHECK(vk_utc_offset_parse("+14:00", &m) && m == 840, "the highest offset");

  const char *bad[] = {"+14:15", "-12:15", "+15:00", "05:30", "+5:30", "+05:3", "+05:30 ", " +05:30",
                       "+05:07", "+05:60", "+0a:00", "UTC", "+05-30", "++5:30", "+", "-", "+05:300", NULL};
  for (int i = 0; bad[i] != NULL; ++i) {
    m = 1234;
    CHECK(!vk_utc_offset_parse(bad[i], &m), "%s must be refused", bad[i]);
    CHECK(m == 1234, "%s: the output was written on a refusal", bad[i]);
  }
  CHECK(!vk_utc_offset_parse(NULL, &m), "NULL is refused");
  CHECK(vk_utc_offset_parse("+01:00", NULL), "a NULL output is allowed");
  CHECK(vk_utc_offset_valid("+01:00") && !vk_utc_offset_valid("+01:01") && vk_utc_offset_valid(""),
        "valid() is parse() without the output");
}

static void test_offset_format(void) {
  char out[VK_UTC_OFFSET_TEXT_CAP];
  vk_utc_offset_format(0, out, sizeof out);
  CHECK(strcmp(out, "+00:00") == 0, "0 -> %s", out);
  vk_utc_offset_format(330, out, sizeof out);
  CHECK(strcmp(out, "+05:30") == 0, "330 -> %s", out);
  vk_utc_offset_format(-240, out, sizeof out);
  CHECK(strcmp(out, "-04:00") == 0, "-240 -> %s", out);
  vk_utc_offset_format(-570, out, sizeof out);
  CHECK(strcmp(out, "-09:30") == 0, "-570 -> %s", out);

  // Every value the stepper can reach round-trips through the text form.
  for (int32_t v = VK_UTC_OFFSET_LOWEST; v <= VK_UTC_OFFSET_HIGHEST; v += VK_UTC_OFFSET_STEP) {
    int32_t back = 9999;
    vk_utc_offset_format(v, out, sizeof out);
    CHECK(vk_utc_offset_parse(out, &back) && back == v, "%d -> %s -> %d", (int)v, out, (int)back);
  }

  char label[VK_UTC_OFFSET_LABEL_CAP];
  vk_utc_offset_label(0, label, sizeof label);
  CHECK(strcmp(label, "UTC") == 0, "label 0 -> %s", label);
  vk_utc_offset_label(330, label, sizeof label);
  CHECK(strcmp(label, "UTC+5:30") == 0, "label 330 -> %s", label);
  vk_utc_offset_label(-240, label, sizeof label);
  CHECK(strcmp(label, "UTC-4") == 0, "label -240 -> %s", label);
  vk_utc_offset_label(840, label, sizeof label);
  CHECK(strcmp(label, "UTC+14") == 0, "label 840 -> %s", label);

  char tiny[3] = "xx";
  vk_utc_offset_format(330, tiny, sizeof tiny);
  CHECK(tiny[sizeof tiny - 1] == '\0', "a short buffer is still terminated");
}

static void test_offset_step(void) {
  CHECK(vk_utc_offset_step(0, 1) == 15, "+15 min");
  CHECK(vk_utc_offset_step(0, -1) == -15, "-15 min");
  CHECK(vk_utc_offset_step(VK_UTC_OFFSET_HIGHEST, 1) == VK_UTC_OFFSET_HIGHEST, "stops at the top");
  CHECK(vk_utc_offset_step(VK_UTC_OFFSET_LOWEST, -1) == VK_UTC_OFFSET_LOWEST, "stops at the bottom");
  CHECK(vk_utc_offset_step(7, 1) == 15 && vk_utc_offset_step(7, -1) == 0, "off the grid: to the neighbour");
  CHECK(vk_utc_offset_step(-7, -1) == -15 && vk_utc_offset_step(-7, 1) == 0, "off the grid, negative");
  CHECK(vk_utc_offset_step(5000, -1) == VK_UTC_OFFSET_HIGHEST, "out of range comes back inside");
  CHECK(vk_utc_offset_step(330, 0) == 330, "direction 0 changes nothing");

  // The choices: every quarter hour from -12:00 to +14:00, ascending, UTC among them.
  const size_t n = vk_utc_offset_count();
  CHECK(n == (size_t)((VK_UTC_OFFSET_HIGHEST - VK_UTC_OFFSET_LOWEST) / VK_UTC_OFFSET_STEP + 1), "count %u", (unsigned)n);
  CHECK(vk_utc_offset_at(0) == VK_UTC_OFFSET_LOWEST && vk_utc_offset_at(n - 1) == VK_UTC_OFFSET_HIGHEST, "ends");
  CHECK(vk_utc_offset_at(n) == 0, "past the end: 0");
  CHECK(vk_utc_offset_at(vk_utc_offset_index(0)) == 0, "index of UTC");
  CHECK(vk_utc_offset_index(330) < n && vk_utc_offset_at(vk_utc_offset_index(330)) == 330, "index of +05:30");
}

static void test_offset_apply(void) {
  // 2026-10-04 12:00:00 UTC.
  const uint32_t noon = 1791115200u;
  CHECK(vk_utc_offset_apply(noon, 0) == noon, "UTC unchanged");
  CHECK(vk_utc_offset_apply(noon, -240) == noon - 14400u, "Detroit in summer is four hours behind");
  CHECK(vk_utc_offset_apply(noon, 330) == noon + 19800u, "India");
  CHECK(vk_utc_offset_apply(100, -240) == 0, "never below 0");
  CHECK(vk_utc_offset_apply(0xFFFFFFF0u, 60) == 0xFFFFFFFFu, "never past the top");

  char hhmm[6];
  vk_utc_offset_clock(noon, -240, hhmm, sizeof hhmm);
  CHECK(strcmp(hhmm, "08:00") == 0, "local 08:00, got %s", hhmm);
  vk_utc_offset_clock(noon + 59, 330, hhmm, sizeof hhmm);
  CHECK(strcmp(hhmm, "17:30") == 0, "local 17:30, got %s", hhmm);
  vk_utc_offset_clock(noon + 12 * 3600 - 60, 0, hhmm, sizeof hhmm);
  CHECK(strcmp(hhmm, "23:59") == 0, "23:59, got %s", hhmm);
}

// ---- number stepper -----------------------------------------------------------------------------

static void test_stepper(void) {
  // A small range steps by one.
  CHECK(vk_step_u32(3, 0, 9, 1) == 4 && vk_step_u32(3, 0, 9, -1) == 2, "small range: by one");
  CHECK(vk_step_u32(9, 0, 9, 1) == 9 && vk_step_u32(0, 0, 9, -1) == 0, "small range: stops at the ends");
  CHECK(vk_step_u32(0, 0, 1, 1) == 1 && vk_step_u32(1, 0, 1, 1) == 1, "a switch");

  // balance_poll_s: 0..3600, default 15.
  CHECK(vk_step_u32(15, 0, 3600, 1) == 20, "15 -> 20, got %u", (unsigned)vk_step_u32(15, 0, 3600, 1));
  CHECK(vk_step_u32(15, 0, 3600, -1) == 10, "15 -> 10");
  CHECK(vk_step_u32(1, 0, 3600, -1) == 0, "down to 0 (off)");
  CHECK(vk_step_u32(0, 0, 3600, 1) == 1, "up from 0");
  CHECK(vk_step_u32(3000, 0, 3600, 1) == 3600 && vk_step_u32(3600, 0, 3600, 1) == 3600, "to the top, then stays");
  CHECK(vk_step_u32(17, 0, 3600, 1) == 20 && vk_step_u32(17, 0, 3600, -1) == 15, "off the ladder: to a neighbour");

  // req_period_ms: 250..5000 -- the range's own ends are stops.
  CHECK(vk_step_u32(250, 250, 5000, -1) == 250, "stays at the bottom");
  CHECK(vk_step_u32(250, 250, 5000, 1) == 300, "250 -> 300, got %u", (unsigned)vk_step_u32(250, 250, 5000, 1));
  CHECK(vk_step_u32(4000, 250, 5000, 1) == 5000, "4000 -> 5000");
  CHECK(vk_step_u32(1000, 250, 5000, -1) == 900, "1000 -> 900");

  // Out of range comes back inside, whatever the direction.
  CHECK(vk_step_u32(9000, 0, 3600, 1) == 3600 && vk_step_u32(9000, 0, 3600, -1) == 3600, "above the range");
  CHECK(vk_step_u32(1, 5, 3600, -1) == 5 && vk_step_u32(1, 5, 3600, 1) == 5, "below the range");
  CHECK(vk_step_u32(30, 0, 3600, 0) == 30, "direction 0");

  // Every step moves and the walk is monotonic, up and down, for every range the keys use.
  const uint32_t ranges[][2] = {{0, 3600}, {15, 3600}, {1, 100}, {0, 5000}, {250, 5000}, {1, 64}, {0, 90},
                                {5, 60}, {0, 4294967295u}};
  for (size_t r = 0; r < sizeof ranges / sizeof ranges[0]; ++r) {
    const uint32_t lo = ranges[r][0], hi = ranges[r][1];
    uint32_t v = lo, steps = 0;
    while (v < hi && steps < 400) {
      const uint32_t next = vk_step_u32(v, lo, hi, 1);
      CHECK(next > v, "range %u..%u: up from %u gave %u", (unsigned)lo, (unsigned)hi, (unsigned)v, (unsigned)next);
      if (next <= v) break;
      v = next;
      ++steps;
    }
    CHECK(v == hi, "range %u..%u: the top was not reached in %u steps", (unsigned)lo, (unsigned)hi, (unsigned)steps);
    CHECK(steps <= 60, "range %u..%u: %u steps from the bottom to the top", (unsigned)lo, (unsigned)hi, (unsigned)steps);
    while (v > lo && steps < 800) {
      const uint32_t next = vk_step_u32(v, lo, hi, -1);
      CHECK(next < v, "range %u..%u: down from %u gave %u", (unsigned)lo, (unsigned)hi, (unsigned)v, (unsigned)next);
      if (next >= v) break;
      v = next;
      ++steps;
    }
    CHECK(v == lo, "range %u..%u: the bottom was not reached", (unsigned)lo, (unsigned)hi);
  }
}

// ---- setup checklist ----------------------------------------------------------------------------

// A new badge: a key, nothing else.
static vk_setup_state_t fresh(void) {
  vk_setup_state_t s;
  memset(&s, 0, sizeof s);
  s.identity = true;
  s.missing_count = 3;
  s.missing[0] = "issuer_key";
  s.missing[1] = "rpc_url";
  s.missing[2] = "tokens";
  s.theme = "light";
  return s;
}

// A badge ready to pay.
static vk_setup_state_t ready(void) {
  vk_setup_state_t s = fresh();
  s.wifi_saved = 1;
  s.wifi_joined = true;
  s.clock = VK_SETUP_CLOCK_SNTP;
  s.provisioned = true;
  s.missing_count = 0;
  s.listener_set = true;
  return s;
}

static const vk_setup_row_t *row(const vk_setup_row_t rows[VK_SETUP_ITEMS], vk_setup_item_t item) {
  for (int i = 0; i < VK_SETUP_ITEMS; ++i) {
    if (rows[i].item == item) return &rows[i];
  }
  return NULL;
}

// Every row of `s`: a label, a value, something to do next, lines that fit the screen.
static void check_shape(const vk_setup_state_t *s) {
  vk_setup_row_t rows[VK_SETUP_ITEMS];
  vk_setup_rows(s, rows);
  int required = 0;
  for (int i = 0; i < VK_SETUP_ITEMS; ++i) {
    CHECK(rows[i].item == (vk_setup_item_t)i, "row %d holds item %d: the order is the enum's", i, (int)rows[i].item);
    CHECK(rows[i].label != NULL && rows[i].label[0] != '\0', "row %d has no label", i);
    CHECK(rows[i].value != NULL, "row %d has no value", i);
    CHECK(rows[i].next[0] != NULL && rows[i].next[0][0] != '\0', "row %d says nothing about what to do next", i);
    for (int k = 0; k < VK_SETUP_NEXT_LINES && rows[i].next[k] != NULL; ++k) {
      CHECK(strlen(rows[i].next[k]) <= VK_SETUP_LINE_MAX, "row %d line %d is %u characters", i, k,
            (unsigned)strlen(rows[i].next[k]));
    }
    CHECK(strlen(rows[i].detail) <= VK_SETUP_LINE_MAX, "row %d detail too long", i);
    int used = rows[i].detail[0] != '\0';
    for (int k = 0; k < VK_SETUP_NEXT_LINES; ++k) used += rows[i].next[k] != NULL;
    CHECK(used <= VK_SETUP_NEXT_LINES, "row %d needs %d lines; the screen has %d", i, used, VK_SETUP_NEXT_LINES);
    if (rows[i].required) ++required;
  }
  CHECK(required == 4, "identity, Wi-Fi, clock and wallet are required: %d", required);
  CHECK(row(rows, VK_SETUP_IDENTITY)->required && row(rows, VK_SETUP_WIFI)->required &&
            row(rows, VK_SETUP_CLOCK)->required && row(rows, VK_SETUP_WALLET)->required,
        "the four required rows");
}

static void test_checklist_shape(void) {
  vk_setup_state_t s = fresh();
  check_shape(&s);
  s.identity = false;
  s.hotspot = true;
  s.clock = VK_SETUP_CLOCK_FLOOR;
  s.missing_count = 0;
  check_shape(&s);
  s = ready();
  check_shape(&s);
  s.wifi_saved = 3;
  s.wifi_joined = false;
  s.name_set = true;
  s.name = "Ayush";
  s.theme = NULL;
  check_shape(&s);
}

static void test_checklist_fresh(void) {
  vk_setup_state_t s = fresh();
  vk_setup_row_t rows[VK_SETUP_ITEMS];
  vk_setup_rows(&s, rows);
  CHECK(vk_setup_left(&s) == 3, "Wi-Fi, clock and wallet left: %u", vk_setup_left(&s));
  CHECK(row(rows, VK_SETUP_IDENTITY)->status == VK_SETUP_DONE, "identity is there");
  CHECK(row(rows, VK_SETUP_WIFI)->status == VK_SETUP_TODO && row(rows, VK_SETUP_WIFI)->go == VK_SETUP_GO_WIFI,
        "Wi-Fi: to do, SELECT opens Wi-Fi");
  CHECK(strcmp(row(rows, VK_SETUP_WIFI)->value, "none") == 0, "Wi-Fi value %s", row(rows, VK_SETUP_WIFI)->value);
  CHECK(row(rows, VK_SETUP_CLOCK)->status == VK_SETUP_TODO && row(rows, VK_SETUP_CLOCK)->go == VK_SETUP_GO_WIFI,
        "no network: the clock row sends the person to Wi-Fi");
  CHECK(row(rows, VK_SETUP_WALLET)->status == VK_SETUP_TODO && row(rows, VK_SETUP_WALLET)->go == VK_SETUP_GO_LAPTOP,
        "the wallet needs the laptop");
  CHECK(strcmp(row(rows, VK_SETUP_WALLET)->detail, "missing: issuer_key, rpc_url, tokens") == 0, "detail: %s",
        row(rows, VK_SETUP_WALLET)->detail);
  CHECK(row(rows, VK_SETUP_NAME)->status == VK_SETUP_OPTIONAL && row(rows, VK_SETUP_NAME)->go == VK_SETUP_GO_NAME,
        "no name: optional, SELECT types it");
  CHECK(strcmp(row(rows, VK_SETUP_NAME)->value, "device name") == 0, "name value %s", row(rows, VK_SETUP_NAME)->value);
  CHECK(row(rows, VK_SETUP_THEME)->go == VK_SETUP_GO_THEME && strcmp(row(rows, VK_SETUP_THEME)->value, "light") == 0,
        "theme row");
  CHECK(row(rows, VK_SETUP_LISTENER)->status == VK_SETUP_OPTIONAL && !row(rows, VK_SETUP_LISTENER)->required,
        "the listener is optional");
  // The wallet text names the tool, never a value.
  int names_tool = 0;
  for (int k = 0; k < VK_SETUP_NEXT_LINES && row(rows, VK_SETUP_WALLET)->next[k]; ++k) {
    if (strstr(row(rows, VK_SETUP_WALLET)->next[k], "vkdev.py provision") != NULL) names_tool = 1;
    CHECK(strstr(row(rows, VK_SETUP_WALLET)->next[k], "http") == NULL, "no address in the instruction");
  }
  CHECK(names_tool, "the wallet row names the provision command");

  CHECK(vk_setup_autoopen(&s, 0), "a new badge opens setup at boot");
  CHECK(!vk_setup_autoopen(&s, 1), "...but not once dismissed");
}

static void test_checklist_progress(void) {
  vk_setup_row_t rows[VK_SETUP_ITEMS];

  // A saved network that is not answering yet: waiting, not to do.
  vk_setup_state_t s = fresh();
  s.wifi_saved = 2;
  vk_setup_rows(&s, rows);
  CHECK(row(rows, VK_SETUP_WIFI)->status == VK_SETUP_WAIT, "saved, not joined: wait");

  // The hotspot is not a network.
  s.hotspot = true;
  vk_setup_rows(&s, rows);
  CHECK(row(rows, VK_SETUP_WIFI)->status == VK_SETUP_TODO && strcmp(row(rows, VK_SETUP_WIFI)->value, "hotspot") == 0,
        "hotspot: to do (%s)", row(rows, VK_SETUP_WIFI)->value);
  s.hotspot = false;

  // Joined, the clock not yet synced: waiting, and SELECT goes to the time page.
  s.wifi_joined = true;
  vk_setup_rows(&s, rows);
  CHECK(row(rows, VK_SETUP_WIFI)->status == VK_SETUP_DONE, "joined");
  CHECK(row(rows, VK_SETUP_CLOCK)->status == VK_SETUP_WAIT && row(rows, VK_SETUP_CLOCK)->go == VK_SETUP_GO_TIME,
        "joined, clock waiting");
  CHECK(vk_setup_left(&s) == 2, "left %u", vk_setup_left(&s));

  // A floor clock (from a record) is not a synced clock: the wallet checks still say unsynced.
  s.clock = VK_SETUP_CLOCK_FLOOR;
  vk_setup_rows(&s, rows);
  CHECK(row(rows, VK_SETUP_CLOCK)->status != VK_SETUP_DONE, "FLOOR does not complete the clock");
  CHECK(strcmp(row(rows, VK_SETUP_CLOCK)->value, "unsynced") == 0, "floor value %s", row(rows, VK_SETUP_CLOCK)->value);
  s.clock = VK_SETUP_CLOCK_SNTP;
  vk_setup_rows(&s, rows);
  CHECK(row(rows, VK_SETUP_CLOCK)->status == VK_SETUP_DONE && strcmp(row(rows, VK_SETUP_CLOCK)->value, "synced") == 0,
        "SNTP completes the clock");

  // Everything set over USB but not committed.
  s.missing_count = 0;
  vk_setup_rows(&s, rows);
  CHECK(row(rows, VK_SETUP_WALLET)->status == VK_SETUP_TODO, "not committed: still to do");
  CHECK(strstr(row(rows, VK_SETUP_WALLET)->detail, "VKCOMMIT") != NULL, "says how to finish: %s",
        row(rows, VK_SETUP_WALLET)->detail);

  // Provisioned: complete, never opens by itself, even with setup_done 0.
  s.provisioned = true;
  vk_setup_rows(&s, rows);
  CHECK(row(rows, VK_SETUP_WALLET)->status == VK_SETUP_DONE, "provisioned");
  CHECK(vk_setup_left(&s) == 0, "nothing left");
  CHECK(!vk_setup_autoopen(&s, 0), "a provisioned badge is not on its first boot");

  // A provisioned badge that is offline still has steps left, but does not open setup at boot.
  s = ready();
  s.wifi_joined = false;
  s.clock = VK_SETUP_CLOCK_NONE;
  CHECK(vk_setup_left(&s) == 2, "offline provisioned badge: %u left", vk_setup_left(&s));
  CHECK(!vk_setup_autoopen(&s, 0), "...and setup does not open by itself");

  // A complete badge does not open either (it cannot be unprovisioned and complete, but the rule
  // must not depend on that).
  s = ready();
  s.provisioned = false;
  s.missing_count = 0;
  CHECK(vk_setup_left(&s) == 1, "only the commit is left");
  s.provisioned = true;
  CHECK(!vk_setup_autoopen(&s, 0), "complete");

  // No identity: to do, and SELECT goes nowhere (never towards New identity).
  s = fresh();
  s.identity = false;
  vk_setup_rows(&s, rows);
  CHECK(row(rows, VK_SETUP_IDENTITY)->status == VK_SETUP_TODO && row(rows, VK_SETUP_IDENTITY)->go == VK_SETUP_GO_NONE,
        "no identity: to do, no jump");
  CHECK(vk_setup_left(&s) == 4, "left %u", vk_setup_left(&s));

  // A name: done; shown as it is.
  s.name_set = true;
  s.name = "Ayush";
  vk_setup_rows(&s, rows);
  CHECK(row(rows, VK_SETUP_NAME)->status == VK_SETUP_DONE && strcmp(row(rows, VK_SETUP_NAME)->value, "Ayush") == 0,
        "name shown");
}

static void test_missing_text(void) {
  vk_setup_state_t s = fresh();
  char out[VK_SETUP_LINE_MAX + 1];
  vk_setup_missing_text(&s, out, sizeof out);
  CHECK(strcmp(out, "missing: issuer_key, rpc_url, tokens") == 0, "%s", out);
  s.missing_count = 1;
  vk_setup_missing_text(&s, out, sizeof out);
  CHECK(strcmp(out, "missing: issuer_key") == 0, "%s", out);
  // More names than fit: cut with "..", never past the buffer.
  s.missing_count = VK_SETUP_MISSING_MAX;
  s.missing[3] = "a_very_long_required_key_name_here";
  vk_setup_missing_text(&s, out, sizeof out);
  CHECK(strlen(out) <= VK_SETUP_LINE_MAX && strstr(out, "..") != NULL, "cut: %s", out);
  char small[12];
  vk_setup_missing_text(&s, small, sizeof small);
  CHECK(strlen(small) < sizeof small, "small buffer: %s", small);
  s.missing_count = 0;
  vk_setup_missing_text(&s, out, sizeof out);
  CHECK(out[0] == '\0', "nothing missing: empty, got %s", out);
  // A count above what the struct holds is clamped.
  s.missing_count = 99;
  vk_setup_missing_text(&s, out, sizeof out);
  CHECK(strlen(out) <= VK_SETUP_LINE_MAX, "clamped");
}

int main(void) {
  test_offset_parse();
  test_offset_format();
  test_offset_step();
  test_offset_apply();
  test_stepper();
  test_checklist_shape();
  test_checklist_fresh();
  test_checklist_progress();
  test_missing_text();
  if (failures) {
    printf("%d failure(s)\n", failures);
    return 1;
  }
  printf("all setup tests passed\n");
  return 0;
}
