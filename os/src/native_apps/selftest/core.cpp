// src/native_apps/selftest/core.cpp
// The runner behind the test suite: rows, the automatic run, manual checks, totals and the serial
// lines. Plain C++ with no Arduino include, so the host simulation compiles this same file (see
// the report of the work package, or apps.md "Self test", for how to run it).
//
// Serial lines (without the "[selftest] " that the app's log function adds):
//   hardware suite (legacyLog)      <name>=<OK|FAIL|--> <value>
//                                   done ok=<n> fail=<n> manual=<n>
//   every other suite               <suite>.<name>=<OK|FAIL|--> <value>
//                                   suite <suite> done ok=<n> fail=<n> manual=<n>
//   RUN ALL, at the end             all done ok=<n> fail=<n> manual=<n>
// "manual" counts every row that is neither OK nor FAIL. A done line is logged when a suite's
// automatic run ends, and again after each manual check of that suite once its run has ended.
#include "selftest.h"

#include <stdio.h>
#include <string.h>

#include <new>

namespace selftest {

const char *stateText(State state) {
  switch (state) {
    case State::Ok:   return "OK";
    case State::Fail: return "FAIL";
    case State::Skip: return "--";
    default:          return "..";
  }
}

// ---- Ctx ---------------------------------------------------------------------------------------

void Ctx::finish(State state, const char *format, ...) {
  va_list args;
  va_start(args, format);
  vsnprintf(value, sizeof value, format, args);
  va_end(args);
  result = state;
  finished = true;
  redraw = true;
}

void Ctx::reset(uint32_t at, size_t row, const char *rowName) {
  now = at;
  step = 0;
  stepAt = at;
  index = row;
  name = rowName ? rowName : "";
  redraw = true;
  finished = false;
  result = State::Waiting;
  value[0] = '\0';
  memset(scratch_, 0, sizeof scratch_);
}

// ---- set-up ------------------------------------------------------------------------------------

namespace {
bool visible(Profile profile, bool dev) { return profile == Profile::Any || dev; }
}  // namespace

bool Runner::begin(const Suite *const *suites, size_t count, bool dev, LogFn log, AllocFn alloc, FreeFn release) {
  dev_ = dev;
  log_ = log;
  alloc_ = alloc;
  free_ = release;
  count_ = 0;
  for (size_t i = 0; i < count && count_ < MAX_SUITES; ++i) {
    const Suite *suite = suites[i];
    if (suite == nullptr || !visible(suite->profile, dev)) continue;
    SuiteRows &slot = suites_[count_];
    slot = SuiteRows();
    slot.suite = suite;
    slot.capacity = suite->rows ? MAX_DYNAMIC_ROWS : suite->count;
    if (slot.capacity > 0) {
      slot.rows = static_cast<Row *>(alloc_(slot.capacity * sizeof(Row)));
      if (slot.rows == nullptr) {
        end();
        return false;
      }
      for (size_t r = 0; r < slot.capacity; ++r) new (&slot.rows[r]) Row();
    }
    buildRows(slot);
    // A suite whose every row belongs to the other profile has nothing to show.
    if (slot.rowCount == 0 && suite->rows == nullptr) {
      if (slot.rows) free_(slot.rows);
      slot = SuiteRows();
      continue;
    }
    ++count_;
  }
  dirty_ = true;
  return true;
}

void Runner::end() {
  for (size_t s = 0; s < count_; ++s) {
    if (suites_[s].suite && suites_[s].suite->stop) suites_[s].suite->stop();
    if (suites_[s].rows && free_) free_(suites_[s].rows);
    suites_[s] = SuiteRows();
  }
  count_ = 0;
  current_ = currentRow_ = -1;
  queueLength_ = 0;
  manualSuite_ = manualRow_ = -1;
  runAll_ = false;
}

// Fills the rows from the table (static) or from rows()/rowName() (dynamic). Results are lost.
void Runner::buildRows(SuiteRows &slot) {
  const Suite &suite = *slot.suite;
  slot.rowCount = 0;
  if (suite.rows) {
    if (suite.count == 0 || suite.checks == nullptr) return;
    size_t n = suite.rows();
    if (n > slot.capacity) {
      logf("%s: %u rows, %u shown", suite.name, (unsigned)n, (unsigned)slot.capacity);
      n = slot.capacity;
    }
    for (size_t i = 0; i < n; ++i) {
      Row &row = slot.rows[i];
      row = Row();
      row.check = &suite.checks[0];
      suite.rowName(i, row.name, sizeof row.name, row.label, sizeof row.label);
    }
    slot.rowCount = n;
    return;
  }
  for (size_t i = 0; i < suite.count; ++i) {
    const Check &check = suite.checks[i];
    if (!visible(check.profile, dev_)) continue;
    Row &row = slot.rows[slot.rowCount++];
    row = Row();
    row.check = &check;
    snprintf(row.name, sizeof row.name, "%s", check.name);
    snprintf(row.label, sizeof row.label, "%s", check.label);
  }
}

// ---- results and lines -------------------------------------------------------------------------

void Runner::logf(const char *format, ...) {
  if (log_ == nullptr) return;
  char line[160];
  va_list args;
  va_start(args, format);
  vsnprintf(line, sizeof line, format, args);
  va_end(args);
  log_(line);
}

void Runner::logRow(size_t s, size_t r) {
  const Suite &suite = *suites_[s].suite;
  const Row &row = suites_[s].rows[r];
  if (suite.legacyLog) logf("%s=%s %s", row.name, stateText(row.state), row.value);
  else logf("%s.%s=%s %s", suite.name, row.name, stateText(row.state), row.value);
}

void Runner::logDone(size_t s) {
  const Counts c = counts(s);
  const Suite &suite = *suites_[s].suite;
  if (suite.legacyLog) logf("done ok=%u fail=%u manual=%u", c.ok, c.fail, c.untested);
  else logf("suite %s done ok=%u fail=%u manual=%u", suite.name, c.ok, c.fail, c.untested);
}

void Runner::record(size_t s, size_t r, const Ctx &c) {
  Row &row = suites_[s].rows[r];
  row.state = c.result == State::Waiting || c.result == State::Running ? State::Fail : c.result;
  snprintf(row.value, sizeof row.value, "%s", c.value);
  logRow(s, r);
  dirty_ = true;
}

Counts Runner::counts(size_t s) const {
  Counts c;
  for (size_t r = 0; r < suites_[s].rowCount; ++r) {
    switch (suites_[s].rows[r].state) {
      case State::Ok:   ++c.ok; break;
      case State::Fail: ++c.fail; break;
      default:          ++c.untested; break;
    }
  }
  return c;
}

Counts Runner::countsAll() const {
  Counts all;
  for (size_t s = 0; s < count_; ++s) {
    const Counts c = counts(s);
    all.ok += c.ok;
    all.fail += c.fail;
    all.untested += c.untested;
  }
  return all;
}

// ---- the automatic run -------------------------------------------------------------------------

void Runner::queue(size_t s) {
  if (s >= count_) return;
  SuiteRows &slot = suites_[s];
  if (slot.queued || current_ == (int)s) return;
  // A manual check of this suite on screen keeps its row: a dynamic suite is not rebuilt then.
  if (slot.suite->rows && manualSuite_ != (int)s) buildRows(slot);
  for (size_t r = 0; r < slot.rowCount; ++r) {
    Row &row = slot.rows[r];
    if (row.check->kind == Kind::Auto) {
      row.state = State::Waiting;
      row.value[0] = '\0';
    } else if (row.state == State::Waiting && !(manualSuite_ == (int)s && manualRow_ == (int)r)) {
      row.state = State::Skip;
      snprintf(row.value, sizeof row.value, "manual");
      if (!slot.manualLogged) logRow(s, r);
    }
  }
  slot.manualLogged = true;
  slot.queued = true;
  queue_[queueLength_++] = (uint8_t)s;
  dirty_ = true;
}

void Runner::queueAll() {
  runAll_ = true;
  for (size_t s = 0; s < count_; ++s) queue(s);
  if (!busy()) {                     // nothing at all to run
    const Counts c = countsAll();
    logf("all done ok=%u fail=%u manual=%u", c.ok, c.fail, c.untested);
    runAll_ = false;
  }
}

int Runner::nextAuto(size_t s, int after) const {
  for (size_t r = (size_t)(after + 1); r < suites_[s].rowCount; ++r) {
    if (suites_[s].rows[r].check->kind == Kind::Auto) return (int)r;
  }
  return -1;
}

void Runner::endSuite() {
  const size_t s = (size_t)current_;
  suites_[s].ran = true;
  logDone(s);
  current_ = currentRow_ = -1;
  dirty_ = true;
  if (runAll_ && queueLength_ == 0) {
    const Counts c = countsAll();
    logf("all done ok=%u fail=%u manual=%u", c.ok, c.fail, c.untested);
    runAll_ = false;
  }
}

void Runner::step(uint32_t now) {
  if (current_ < 0) {
    if (queueLength_ == 0) return;
    current_ = queue_[0];
    memmove(queue_, queue_ + 1, --queueLength_);
    suites_[current_].queued = false;
    currentRow_ = nextAuto((size_t)current_, -1);
    if (currentRow_ < 0) {           // a suite of manual rows only, or an empty dynamic suite
      endSuite();
      return;
    }
  }

  const size_t s = (size_t)current_, r = (size_t)currentRow_;
  Row &row = suites_[s].rows[r];
  if (row.state == State::Waiting) {
    row.state = State::Running;
    autoCtx_.reset(now, r, row.name);
    autoStartedAt_ = now;
    dirty_ = true;
  }
  autoCtx_.now = now;
  if (row.check->run) {
    row.check->run(autoCtx_);
  } else {
    autoCtx_.finish(State::Fail, "no check");
  }
  const uint32_t limit = row.check->timeoutMs ? row.check->timeoutMs : DEFAULT_TIMEOUT_MS;
  if (!autoCtx_.finished && (uint32_t)(now - autoStartedAt_) > limit) {
    autoCtx_.finish(State::Fail, "no result in %lu s", (unsigned long)(limit / 1000));
  }
  if (autoCtx_.redraw) {
    dirty_ = true;
    autoCtx_.redraw = false;
  }
  if (!autoCtx_.finished) return;

  record(s, r, autoCtx_);
  currentRow_ = nextAuto(s, currentRow_);
  if (currentRow_ < 0) endSuite();
}

// ---- manual checks -----------------------------------------------------------------------------

bool Runner::manualStart(size_t s, size_t r, uint32_t now) {
  if (manualActive() || s >= count_ || r >= suites_[s].rowCount) return false;
  Row &row = suites_[s].rows[r];
  if (row.check->kind != Kind::Manual || row.check->manual == nullptr) return false;
  manualSuite_ = (int)s;
  manualRow_ = (int)r;
  row.state = State::Running;
  manualCtx_.reset(now, r, row.name);
  dirty_ = true;
  if (row.check->manual->start) row.check->manual->start(manualCtx_);
  manualUpdate(now);                 // a start that finished at once ends here
  return true;
}

const Check *Runner::manualCheck() const {
  if (!manualActive()) return nullptr;
  return suites_[manualSuite_].rows[manualRow_].check;
}

void Runner::manualUpdate(uint32_t now) {
  if (!manualActive()) return;
  manualCtx_.now = now;
  const ManualOps *ops = manualCheck()->manual;
  if (!manualCtx_.finished && ops->update) ops->update(manualCtx_);
  if (manualCtx_.redraw) {
    dirty_ = true;
    manualCtx_.redraw = false;
  }
  if (!manualCtx_.finished) return;

  const size_t s = (size_t)manualSuite_, r = (size_t)manualRow_;
  manualSuite_ = manualRow_ = -1;
  record(s, r, manualCtx_);
  // The totals follow once the suite's own automatic run has logged them.
  if (suites_[s].ran && !suites_[s].queued && current_ != (int)s) logDone(s);
  manualEnded_ = true;
  endedSuite_ = s;
  endedRow_ = r;
  endedState_ = suites_[s].rows[r].state;
}

void Runner::manualDraw() {
  if (!manualActive()) return;
  const ManualOps *ops = manualCheck()->manual;
  if (ops->draw) ops->draw(manualCtx_);
}

void Runner::manualKey(uint8_t key, uint32_t now) {
  if (!manualActive()) return;
  manualCtx_.now = now;
  const ManualOps *ops = manualCheck()->manual;
  if (ops->key) ops->key(manualCtx_, key);
  manualCtx_.redraw = true;
  manualUpdate(now);
}

bool Runner::takeManualEnd(size_t &s, size_t &r, State &state) {
  if (!manualEnded_) return false;
  manualEnded_ = false;
  s = endedSuite_;
  r = endedRow_;
  state = endedState_;
  return true;
}

}  // namespace selftest
