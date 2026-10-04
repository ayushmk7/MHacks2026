// test/host/shim/Arduino.h
// Host-test stand-in for <Arduino.h>. On the include path of every C++ host suite (run.sh passes
// -Itest/host/shim), so a firmware file compiled with -DVK_HOST_TEST gets this instead of the
// Arduino core. It provides only what host-tested code may use:
//   - the fixed-width types and the C headers Arduino.h pulls in,
//   - String (built on std::string, with Arduino's method names and semantics),
//   - millis() from a fake clock that the test sets with vk_host_set_millis(),
//   - strlcpy / strlcat.
// Hardware (Serial, pins, Wi-Fi, the display, ...) is deliberately absent: host-tested code keeps
// those calls behind #ifndef VK_HOST_TEST or behind function pointers (testing.md, Host tests).
#pragma once

#include <math.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ---- the fake clock -----------------------------------------------------------------------
#ifdef __cplusplus
extern "C" {
#endif
unsigned long millis(void);                 // the value last set; starts at 0
unsigned long micros(void);                 // millis() * 1000
void delay(unsigned long ms);               // advances the fake clock by ms; does not sleep
void yield(void);                           // does nothing
void vk_host_set_millis(uint32_t ms);       // test hook: set what millis() returns (wraps like the badge's, at 2^32)
#ifdef __cplusplus
}
#endif

// ---- strlcpy / strlcat ----------------------------------------------------------------------
// macOS and the BSDs have them in <string.h>, and so does glibc from 2.38. Elsewhere, define them.
#if !defined(__APPLE__) && !defined(__FreeBSD__) && !defined(__OpenBSD__) && !defined(__NetBSD__) && \
    !(defined(__GLIBC__) && (__GLIBC__ > 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ >= 38)))
static inline size_t strlcpy(char *dst, const char *src, size_t size) {
  const size_t n = strlen(src);
  if (size) { const size_t k = n < size - 1 ? n : size - 1; memcpy(dst, src, k); dst[k] = '\0'; }
  return n;
}
static inline size_t strlcat(char *dst, const char *src, size_t size) {
  const size_t d = strnlen(dst, size);
  if (d == size) return size + strlen(src);
  return d + strlcpy(dst + d, src, size - d);
}
#endif

// ---- small Arduino names ----------------------------------------------------------------------
typedef uint8_t byte;
typedef bool boolean;
#define DEC 10
#define HEX 16
#define OCT 8
#define BIN 2
#define PROGMEM
#define PSTR(s) (s)
#define F(s) (s)

#ifdef __cplusplus
#include <algorithm>
#include <string>
using std::max;
using std::min;

// ---- String -------------------------------------------------------------------------------
// Arduino's String on top of std::string. Same names and return types as the core's WString.h for
// the methods below; indexes are clamped the way the core clamps them. Not provided: the
// StringSumHelper type, flash-string (F()) overloads beyond plain const char*, move-only helpers.
class String {
 public:
  String() {}
  String(const char *cstr) : s_(cstr ? cstr : "") {}
  String(const char *cstr, unsigned int length) : s_(cstr ? std::string(cstr, length) : std::string()) {}
  String(const std::string &s) : s_(s) {}
  String(const String &) = default;
  String(String &&) = default;
  explicit String(char c) : s_(1, c) {}
  explicit String(unsigned char value, unsigned char base = 10) : s_(fromUnsigned(value, base)) {}
  explicit String(int value, unsigned char base = 10) : s_(fromSigned(value, base)) {}
  explicit String(unsigned int value, unsigned char base = 10) : s_(fromUnsigned(value, base)) {}
  explicit String(long value, unsigned char base = 10) : s_(fromSigned(value, base)) {}
  explicit String(unsigned long value, unsigned char base = 10) : s_(fromUnsigned(value, base)) {}
  explicit String(long long value, unsigned char base = 10) : s_(fromSigned(value, base)) {}
  explicit String(unsigned long long value, unsigned char base = 10) : s_(fromUnsigned(value, base)) {}
  explicit String(float value, unsigned int decimalPlaces = 2) : s_(fromDouble(value, decimalPlaces)) {}
  explicit String(double value, unsigned int decimalPlaces = 2) : s_(fromDouble(value, decimalPlaces)) {}

  String &operator=(const String &) = default;
  String &operator=(String &&) = default;
  String &operator=(const char *cstr) { s_ = cstr ? cstr : ""; return *this; }

  // -- size and storage
  unsigned int length() const { return (unsigned int)s_.size(); }
  bool isEmpty() const { return s_.empty(); }
  const char *c_str() const { return s_.c_str(); }
  const char *begin() const { return s_.c_str(); }
  const char *end() const { return s_.c_str() + s_.size(); }
  bool reserve(unsigned int size) { s_.reserve(size); return true; }
  void clear() { s_.clear(); }
  const std::string &std_str() const { return s_; }      // shim only: for tests

  // -- concat. Returns true like the core (it reports allocation success there).
  bool concat(const String &str) { s_ += str.s_; return true; }
  bool concat(const char *cstr) { if (cstr) s_ += cstr; return cstr != nullptr; }
  bool concat(const char *cstr, unsigned int length) { if (cstr) s_.append(cstr, length); return cstr != nullptr; }
  bool concat(char c) { s_ += c; return true; }
  bool concat(unsigned char num) { s_ += fromUnsigned(num, 10); return true; }
  bool concat(int num) { s_ += fromSigned(num, 10); return true; }
  bool concat(unsigned int num) { s_ += fromUnsigned(num, 10); return true; }
  bool concat(long num) { s_ += fromSigned(num, 10); return true; }
  bool concat(unsigned long num) { s_ += fromUnsigned(num, 10); return true; }
  bool concat(long long num) { s_ += fromSigned(num, 10); return true; }
  bool concat(unsigned long long num) { s_ += fromUnsigned(num, 10); return true; }
  bool concat(float num) { s_ += fromDouble(num, 2); return true; }
  bool concat(double num) { s_ += fromDouble(num, 2); return true; }

  String &operator+=(const String &rhs) { concat(rhs); return *this; }
  String &operator+=(const char *cstr) { concat(cstr); return *this; }
  String &operator+=(char c) { concat(c); return *this; }
  String &operator+=(unsigned char num) { concat(num); return *this; }
  String &operator+=(int num) { concat(num); return *this; }
  String &operator+=(unsigned int num) { concat(num); return *this; }
  String &operator+=(long num) { concat(num); return *this; }
  String &operator+=(unsigned long num) { concat(num); return *this; }
  String &operator+=(long long num) { concat(num); return *this; }
  String &operator+=(unsigned long long num) { concat(num); return *this; }
  String &operator+=(float num) { concat(num); return *this; }
  String &operator+=(double num) { concat(num); return *this; }

  // -- comparison
  int compareTo(const String &other) const { return s_.compare(other.s_); }
  bool equals(const String &other) const { return s_ == other.s_; }
  bool equals(const char *cstr) const { return cstr ? s_ == cstr : s_.empty(); }
  bool equalsIgnoreCase(const String &other) const {
    if (s_.size() != other.s_.size()) return false;
    for (size_t i = 0; i < s_.size(); i++) if (lower(s_[i]) != lower(other.s_[i])) return false;
    return true;
  }
  bool operator==(const String &rhs) const { return equals(rhs); }
  bool operator==(const char *cstr) const { return equals(cstr); }
  bool operator!=(const String &rhs) const { return !equals(rhs); }
  bool operator!=(const char *cstr) const { return !equals(cstr); }
  bool operator<(const String &rhs) const { return s_ < rhs.s_; }
  bool operator>(const String &rhs) const { return s_ > rhs.s_; }
  bool operator<=(const String &rhs) const { return s_ <= rhs.s_; }
  bool operator>=(const String &rhs) const { return s_ >= rhs.s_; }

  bool startsWith(const String &prefix) const { return startsWith(prefix, 0); }
  bool startsWith(const String &prefix, unsigned int offset) const {
    if (offset > s_.size() || prefix.s_.size() > s_.size() - offset) return false;
    return s_.compare(offset, prefix.s_.size(), prefix.s_) == 0;
  }
  bool endsWith(const String &suffix) const {
    if (suffix.s_.size() > s_.size()) return false;
    return s_.compare(s_.size() - suffix.s_.size(), suffix.s_.size(), suffix.s_) == 0;
  }

  // -- characters. Out-of-range reads return 0; out-of-range writes go to a dummy, like the core.
  char charAt(unsigned int index) const { return index < s_.size() ? s_[index] : 0; }
  void setCharAt(unsigned int index, char c) { if (index < s_.size()) s_[index] = c; }
  char operator[](unsigned int index) const { return charAt(index); }
  char &operator[](unsigned int index) {
    static char dummy;
    if (index >= s_.size()) { dummy = 0; return dummy; }
    return s_[index];
  }
  void getBytes(unsigned char *buf, unsigned int bufsize, unsigned int index = 0) const {
    if (!buf || !bufsize) return;
    if (index >= s_.size()) { buf[0] = 0; return; }
    unsigned int n = bufsize - 1;
    if (n > s_.size() - index) n = (unsigned int)(s_.size() - index);
    memcpy(buf, s_.data() + index, n);
    buf[n] = 0;
  }
  void toCharArray(char *buf, unsigned int bufsize, unsigned int index = 0) const {
    getBytes((unsigned char *)buf, bufsize, index);
  }

  // -- search. -1 when not found.
  int indexOf(char ch) const { return indexOf(ch, 0); }
  int indexOf(char ch, unsigned int fromIndex) const {
    if (fromIndex >= s_.size()) return -1;
    const size_t at = s_.find(ch, fromIndex);
    return at == std::string::npos ? -1 : (int)at;
  }
  int indexOf(const String &str) const { return indexOf(str, 0); }
  int indexOf(const String &str, unsigned int fromIndex) const {
    if (fromIndex >= s_.size()) return -1;
    const size_t at = s_.find(str.s_, fromIndex);
    return at == std::string::npos ? -1 : (int)at;
  }
  int lastIndexOf(char ch) const { return s_.empty() ? -1 : lastIndexOf(ch, (unsigned int)s_.size() - 1); }
  int lastIndexOf(char ch, unsigned int fromIndex) const {
    if (fromIndex >= s_.size()) return -1;
    const size_t at = s_.rfind(ch, fromIndex);
    return at == std::string::npos ? -1 : (int)at;
  }
  int lastIndexOf(const String &str) const {
    if (str.s_.empty() || str.s_.size() > s_.size()) return -1;
    return lastIndexOf(str, (unsigned int)(s_.size() - str.s_.size()));
  }
  int lastIndexOf(const String &str, unsigned int fromIndex) const {
    if (str.s_.empty() || s_.empty() || str.s_.size() > s_.size()) return -1;
    const size_t at = s_.rfind(str.s_, fromIndex);
    return at == std::string::npos ? -1 : (int)at;
  }

  // -- pieces. substring(begin, end) excludes end; swapped arguments are swapped back; end is clamped.
  String substring(unsigned int beginIndex) const { return substring(beginIndex, (unsigned int)s_.size()); }
  String substring(unsigned int beginIndex, unsigned int endIndex) const {
    if (beginIndex > endIndex) { const unsigned int t = beginIndex; beginIndex = endIndex; endIndex = t; }
    if (beginIndex >= s_.size()) return String();
    if (endIndex > s_.size()) endIndex = (unsigned int)s_.size();
    return String(s_.substr(beginIndex, endIndex - beginIndex));
  }

  // -- in-place edits
  void replace(char find, char with) { for (char &c : s_) if (c == find) c = with; }
  void replace(const String &find, const String &with) {
    if (find.s_.empty()) return;
    size_t at = 0;
    while ((at = s_.find(find.s_, at)) != std::string::npos) { s_.replace(at, find.s_.size(), with.s_); at += with.s_.size(); }
  }
  void remove(unsigned int index) { if (index < s_.size()) s_.erase(index); }
  void remove(unsigned int index, unsigned int count) { if (index < s_.size() && count) s_.erase(index, count); }
  void toLowerCase() { for (char &c : s_) c = lower(c); }
  void toUpperCase() { for (char &c : s_) c = (c >= 'a' && c <= 'z') ? (char)(c - 'a' + 'A') : c; }
  void trim() {
    size_t a = 0, b = s_.size();
    while (a < b && space(s_[a])) a++;
    while (b > a && space(s_[b - 1])) b--;
    s_ = s_.substr(a, b - a);
  }

  // -- numbers. 0 when the text does not start with a number, like atol / atof.
  long toInt() const { return atol(s_.c_str()); }
  float toFloat() const { return (float)atof(s_.c_str()); }
  double toDouble() const { return atof(s_.c_str()); }

 private:
  static char lower(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c; }
  static bool space(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f'; }
  static std::string fromUnsigned(unsigned long long value, unsigned char base) {
    if (base < 2 || base > 36) base = 10;
    char buf[65]; size_t at = sizeof buf; buf[--at] = 0;
    do { const unsigned d = (unsigned)(value % base); buf[--at] = (char)(d < 10 ? '0' + d : 'a' + d - 10); value /= base; } while (value);
    return std::string(buf + at);
  }
  static std::string fromSigned(long long value, unsigned char base) {
    if (value < 0 && base == 10) return "-" + fromUnsigned(0ULL - (unsigned long long)value, 10);
    return fromUnsigned((unsigned long long)value, base);
  }
  static std::string fromDouble(double value, unsigned int decimalPlaces) {
    char buf[64];
    snprintf(buf, sizeof buf, "%.*f", (int)decimalPlaces, value);
    return std::string(buf);
  }
  std::string s_;
};

inline String operator+(const String &lhs, const String &rhs) { String r(lhs); r += rhs; return r; }
inline String operator+(const String &lhs, const char *rhs) { String r(lhs); r += rhs; return r; }
inline String operator+(const char *lhs, const String &rhs) { String r(lhs); r += rhs; return r; }
inline String operator+(const String &lhs, char rhs) { String r(lhs); r += rhs; return r; }
inline String operator+(char lhs, const String &rhs) { String r(lhs); r += rhs; return r; }
inline String operator+(const String &lhs, unsigned char rhs) { String r(lhs); r += rhs; return r; }
inline String operator+(const String &lhs, int rhs) { String r(lhs); r += rhs; return r; }
inline String operator+(const String &lhs, unsigned int rhs) { String r(lhs); r += rhs; return r; }
inline String operator+(const String &lhs, long rhs) { String r(lhs); r += rhs; return r; }
inline String operator+(const String &lhs, unsigned long rhs) { String r(lhs); r += rhs; return r; }
inline String operator+(const String &lhs, long long rhs) { String r(lhs); r += rhs; return r; }
inline String operator+(const String &lhs, unsigned long long rhs) { String r(lhs); r += rhs; return r; }
inline String operator+(const String &lhs, float rhs) { String r(lhs); r += rhs; return r; }
inline String operator+(const String &lhs, double rhs) { String r(lhs); r += rhs; return r; }
inline bool operator==(const char *lhs, const String &rhs) { return rhs == lhs; }
inline bool operator!=(const char *lhs, const String &rhs) { return rhs != lhs; }

#endif  // __cplusplus
