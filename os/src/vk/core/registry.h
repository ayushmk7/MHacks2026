#pragma once
namespace vk {
// Derive T from Registered<T>. Every static T links itself into one list before setup() runs.
template <class T>
class Registered {
 public:
  Registered() : next_(head()) { head() = static_cast<T *>(this); }
  static T *first() { return head(); }
  T *next() const { return next_; }
 private:
  static T *&head() { static T *h = nullptr; return h; }   // function-local: no init-order problem
  T *next_;
};
}  // namespace vk
