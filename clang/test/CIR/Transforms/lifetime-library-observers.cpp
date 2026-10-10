// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -std=c++20 -fclangir -fclangir-lifetime-check="history=all" -verify -emit-cir %s -o %t.cir
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -std=c++20 -fclangir -fclangir-lifetime-check -clangir-disable-passes -emit-cir %s -o %t.raw.cir
// RUN: cir-opt %t.raw.cir --cir-canonicalize --cir-lifetime-check="history=all" -o /dev/null 2>&1 | FileCheck %s --allow-empty
// expected-no-diagnostics
// CHECK-NOT: warning:
// CHECK-NOT: error:

namespace std {
template <class T> T &&move(T &value) { return static_cast<T &&>(value); }
template <class T> class [[gsl::Owner(T)]] shared_ptr {
public:
  explicit shared_ptr(T *) noexcept;
  shared_ptr(shared_ptr &&) noexcept;
  shared_ptr(const shared_ptr &) noexcept;
  void reset() noexcept;
  T *get() const noexcept;
  explicit operator bool() const noexcept;
  T &operator*() const noexcept;
};
template <class T> class weak_ptr {
public:
  weak_ptr() noexcept;
  weak_ptr(weak_ptr &&) noexcept;
  void reset() noexcept;
  bool expired() const noexcept;
  shared_ptr<T> lock() const noexcept;
};
template <class T> class optional {
public:
  optional(T) noexcept;
  optional(optional &&) noexcept;
  bool has_value() const noexcept;
  void reset() noexcept;
};
class any {
public:
  any(int) noexcept;
  any(any &&) noexcept;
  bool has_value() const noexcept;
  void reset() noexcept;
};
}

void smart_move_and_reset_observers() {
  std::shared_ptr<int> source(new int(1));
  std::shared_ptr<int> moved(std::move(source));
  (void)source.get();
  if (source)
    (void)*source;
  source.reset();
  (void)source.get();
}

void weak_move_and_reset_observers() {
  std::weak_ptr<int> source;
  std::weak_ptr<int> moved(std::move(source));
  (void)source.expired();
  auto locked = source.lock();
  if (locked)
    (void)*locked;
  source.reset();
  (void)source.expired();
}

void optional_move_preserves_engagement() {
  std::optional<int> source(1);
  std::optional<int> moved(std::move(source));
  (void)source.has_value();
  source.reset();
  (void)source.has_value();
}

void any_move_and_reset_observers() {
  std::any source(1);
  std::any moved(std::move(source));
  (void)source.has_value();
  source.reset();
  (void)source.has_value();
}
