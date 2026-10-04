// src/native_apps/selftest/selftest.h
// The test suite's table types and its runner (docs/os/apps/apps.md, "Self test").
//
// A suite is a table of checks. A check is one row of a checklist: a name for the serial log, a
// label for the screen, automatic or manual, the profile it exists in, and its function. Adding a
// check is adding one entry to the table in its suite's file (suite_<name>.cpp); adding a suite is
// one new file and one line in suites.cpp.
//
// Two kinds of suite:
//   static    `checks` is the table, one row per entry
//   dynamic   `rows()` says how many rows there are (one per installed app, one per test vector)
//             and `rowName()` names each; every row is run by checks[0], which reads Ctx::index
//
// This header and core.cpp are plain C++ (no Arduino), so the runner is also compiled on the
// laptop by the host simulation (see core.cpp).
#pragma once

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

namespace selftest {

constexpr char TAG[] = "selftest";       // every log line is "[selftest] ..."

enum class State : uint8_t { Waiting, Running, Ok, Fail, Skip };   // Skip is shown as "--"
enum class Kind : uint8_t { Auto, Manual };
enum class Profile : uint8_t { Any, Dev };   // Dev: the row exists only when VK_PROFILE_DEV is 1

constexpr size_t VALUE_MAX = 40;         // a result's value, with its NUL
constexpr size_t NAME_MAX = 33;          // a row's serial name: an app id is up to 32 characters
constexpr size_t LABEL_MAX = 20;         // a row's label on the screen
constexpr size_t SCRATCH_BYTES = 128;    // what one check may keep between its steps

const char *stateText(State state);      // "OK", "FAIL", "--", ".."

// What a check function sees. Automatic checks get a fresh Ctx (zeroed scratch, step 0) when they
// start and are called once per frame until they call finish(); manual checks the same, through
// their ManualOps, until finish().
struct Ctx {
  uint32_t now = 0;                      // millis() of this frame
  uint8_t step = 0;                      // free for the check: which of its steps comes next
  uint32_t stepAt = 0;                   // free for the check: when that step began (set to `now` at start)
  size_t index = 0;                      // the row: its index in its suite
  const char *name = "";                 // the row's serial name (an app id in the APPS suite)
  bool redraw = false;                   // ask for a repaint (the runner clears it after reading)

  // Ends the check. Formats the value shown on the row and logged after the state.
  void finish(State state, const char *format, ...) __attribute__((format(printf, 3, 4)));

  // A check's own state between steps: one struct of its own, zeroed at start.
  template <class T>
  T &scratch() {
    static_assert(sizeof(T) <= SCRATCH_BYTES, "a check's scratch is at most SCRATCH_BYTES");
    return *reinterpret_cast<T *>(scratch_);
  }

  // ---- set by finish(), read by the runner ----
  bool finished = false;
  State result = State::Waiting;
  char value[VALUE_MAX] = "";

  void reset(uint32_t at, size_t row, const char *rowName);

 private:
  alignas(8) uint8_t scratch_[SCRATCH_BYTES] = {};
};

// A manual check has a screen of its own while it runs. All four are called only while it is the
// manual check in progress; `key` gets presses only (no releases).
struct ManualOps {
  void (*start)(Ctx &c);                 // may finish() at once (nothing to test, a refusal)
  void (*update)(Ctx &c);                // every frame
  void (*draw)(Ctx &c);                  // when a repaint is due; draws the whole screen
  void (*key)(Ctx &c, uint8_t key);
  bool fullScreen;                       // true: no once-a-second repaint (the display test's screens)
};

struct Check {
  const char *name;                      // serial name; "" in a dynamic suite (rowName() names the rows)
  const char *label;                     // on the screen
  Kind kind;
  Profile profile;
  void (*run)(Ctx &c);                   // automatic: one step; nullptr for a manual check
  const ManualOps *manual;               // manual: its screen; nullptr for an automatic check
  uint32_t timeoutMs;                    // 0: DEFAULT_TIMEOUT_MS. The runner ends a check that takes longer
};

constexpr uint32_t DEFAULT_TIMEOUT_MS = 20000;

struct Suite {
  const char *name;                      // serial: "<name>.<check>=..." and "suite <name> done ..."
  const char *title;                     // on the menu and as the checklist's title
  Profile profile;
  const Check *checks;
  size_t count;
  // Dynamic suites only (nullptr otherwise).
  size_t (*rows)();
  void (*rowName)(size_t index, char *name, size_t nameCap, char *label, size_t labelCap);
  // Gives back what the suite's checks took (microphone, LEDs, a Wi-Fi scan) and leaves its own
  // file-level state as at boot. Called when the app stops. May be nullptr.
  void (*stop)();
  // The hardware suite logs as the first Self test did: "<name>=..." without a suite prefix, and
  // "done ok=.." instead of "suite <name> done ok=..". t_selftest.py and older logs read that.
  bool legacyLog;
};

struct Row {
  const Check *check = nullptr;
  State state = State::Waiting;
  char name[NAME_MAX] = "";
  char label[LABEL_MAX] = "";
  char value[VALUE_MAX] = "";
};

struct Counts {
  unsigned ok = 0, fail = 0, untested = 0;   // untested: every row that is not OK or FAIL
};

constexpr size_t MAX_SUITES = 12;
constexpr size_t MAX_DYNAMIC_ROWS = 40;      // a dynamic suite shows at most this many rows

// Runs suites. Owns the rows; knows nothing about the screen or the hardware.
class Runner {
 public:
  using LogFn = void (*)(const char *line);  // one line without the "[selftest] " prefix
  using AllocFn = void *(*)(size_t bytes);
  using FreeFn = void (*)(void *memory);

  // `dev`: VK_PROFILE_DEV. Suites and checks of the dev profile are left out when it is false.
  bool begin(const Suite *const *suites, size_t count, bool dev, LogFn log, AllocFn alloc, FreeFn release);
  void end();                                 // frees the rows; calls every suite's stop()

  size_t suiteCount() const { return count_; }
  const Suite &suite(size_t s) const { return *suites_[s].suite; }
  size_t rowCount(size_t s) const { return suites_[s].rowCount; }
  const Row &row(size_t s, size_t r) const { return suites_[s].rows[r]; }
  Counts counts(size_t s) const;
  Counts countsAll() const;

  // The automatic run. queue() (re)runs every automatic row of one suite: rows go back to waiting,
  // and the first time a suite is queued its manual rows are logged as "--" (manual). A suite that
  // is already waiting or running is left alone. queueAll() is RUN ALL: every suite in turn, then
  // "all done ok=.. fail=.. manual=..".
  void queue(size_t s);
  void queueAll();
  bool queued(size_t s) const { return suites_[s].queued; }
  bool running(size_t s) const { return current_ == (int)s; }
  bool ran(size_t s) const { return suites_[s].ran; }
  bool runAllActive() const { return runAll_; }
  bool busy() const { return current_ >= 0 || queueLength_ > 0; }
  void step(uint32_t now);                    // one step of the current automatic check

  // A manual check: the app shows its screen until it finishes.
  bool manualStart(size_t s, size_t r, uint32_t now);
  bool manualActive() const { return manualSuite_ >= 0; }
  const Check *manualCheck() const;
  void manualUpdate(uint32_t now);
  void manualDraw();
  void manualKey(uint8_t key, uint32_t now);
  // After a manual check finished: where it was and how it ended (for the cursor), then cleared.
  bool takeManualEnd(size_t &s, size_t &r, State &state);

  bool takeDirty() { const bool d = dirty_; dirty_ = false; return d; }

 private:
  struct SuiteRows {
    const Suite *suite = nullptr;
    Row *rows = nullptr;
    size_t rowCount = 0;
    size_t capacity = 0;
    bool queued = false;
    bool ran = false;          // an automatic run of it has ended at least once
    bool manualLogged = false; // its manual rows were logged as "--" once
  };

  void buildRows(SuiteRows &suite);
  void record(size_t s, size_t r, const Ctx &c);
  void logRow(size_t s, size_t r);
  void logDone(size_t s);
  void logf(const char *format, ...) __attribute__((format(printf, 2, 3)));
  int nextAuto(size_t s, int after) const;
  void endSuite();

  SuiteRows suites_[MAX_SUITES];
  size_t count_ = 0;
  bool dev_ = true;
  LogFn log_ = nullptr;
  AllocFn alloc_ = nullptr;
  FreeFn free_ = nullptr;

  uint8_t queue_[MAX_SUITES] = {};
  size_t queueLength_ = 0;
  int current_ = -1;           // the suite whose automatic rows are running
  int currentRow_ = -1;
  Ctx autoCtx_;
  uint32_t autoStartedAt_ = 0;
  bool runAll_ = false;

  int manualSuite_ = -1, manualRow_ = -1;
  Ctx manualCtx_;
  bool manualEnded_ = false;
  size_t endedSuite_ = 0, endedRow_ = 0;
  State endedState_ = State::Waiting;

  bool dirty_ = true;
};

// suites.cpp: the suites, in menu order.
extern const Suite *const SUITES[];
extern const size_t SUITE_COUNT;

}  // namespace selftest
