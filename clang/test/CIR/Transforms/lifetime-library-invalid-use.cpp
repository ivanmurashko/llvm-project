// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -std=c++20 -fclangir -fclangir-lifetime-check="history=all" -verify -emit-cir %s -o %t.cir
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -std=c++20 -fclangir -fclangir-lifetime-check -clangir-disable-passes -emit-cir %s -o %t.raw.cir
// RUN: cir-opt %t.raw.cir --cir-canonicalize --cir-lifetime-check="history=all" -o /dev/null 2>&1 | FileCheck %s
// RUN: cir-opt %t.raw.cir --emit-bytecode -o %t.bc
// RUN: cir-opt %t.bc --cir-lifetime-check="history=all" -o /dev/null 2>&1 | FileCheck %s

namespace std {
struct nullopt_t {};
inline constexpr nullopt_t nullopt;
template <class T> T &&move(T &value) { return static_cast<T &&>(value); }
template <class T> class [[gsl::Owner(T)]] unique_ptr {
public:
  explicit unique_ptr(T *) noexcept;
  unique_ptr(unique_ptr &&) noexcept;
  void reset(T * = nullptr) noexcept;
  T *get() const noexcept;
  T &operator*() const noexcept;
};
template <class T> class optional {
public:
  optional() noexcept;
  optional(T) noexcept;
  optional(nullopt_t) noexcept;
  optional &operator=(nullopt_t) noexcept;
  void reset() noexcept;
  bool has_value() const noexcept;
  T &operator*() & noexcept;
};
template <class T> class vector {
public:
  explicit vector(unsigned);
  void clear() noexcept;
  bool empty() const noexcept;
  T &front() noexcept;
};
} // namespace std

int null_reset_is_not_nonnull() {
  std::unique_ptr<int> null_reset(new int(1));
  std::unique_ptr<int> destination(std::move(null_reset));
  null_reset.reset(); // expected-note {{'nullptr' invalidated here}}
  return *null_reset; // expected-warning {{use of invalid pointer 'null_reset'}}
}

int optional_reset_disengages() {
  std::optional<int> disengaged(42);
  disengaged.reset(); // expected-note {{'nullptr' invalidated here}}
  return *disengaged; // expected-warning {{use of invalid pointer 'disengaged'}}
}

void optional_guard_is_safe() {
  std::optional<int> guarded(42);
  guarded.reset();
  if (guarded.has_value())
    (void)*guarded;
}

int clear_does_not_make_front_valid() {
  std::vector<int> cleared(1);
  cleared.clear(); // expected-note {{'nullptr' invalidated here}}
  return cleared.front(); // expected-warning {{use of invalid pointer 'cleared'}}
}

void empty_container_guard_is_safe() {
  std::vector<int> guarded(1);
  guarded.clear();
  if (!guarded.empty())
    (void)guarded.front();
}

int reset_invalidates_old_borrow() {
  std::unique_ptr<int> owner(new int(1));
  int *old = owner.get();
  owner.reset(new int(2)); // expected-note {{invalidated by non-const use of owner}}
  return *old; // expected-warning {{use of invalid pointer 'old'}}
}

int nullopt_construction_is_empty() {
  std::optional<int> absent(std::nullopt); // expected-note {{'nullptr' invalidated here}}
  return *absent; // expected-warning {{use of invalid pointer 'absent'}}
}

int nullopt_assignment_is_empty() {
  std::optional<int> absent(42);
  absent = std::nullopt; // expected-note {{'nullptr' invalidated here}}
  return *absent; // expected-warning {{use of invalid pointer 'absent'}}
}

void nullopt_guard_is_safe() {
  std::optional<int> absent(std::nullopt);
  if (absent.has_value())
    (void)*absent;
}

// CHECK: warning: use of invalid pointer 'null_reset'
// CHECK: warning: use of invalid pointer 'disengaged'
// CHECK: warning: use of invalid pointer 'cleared'
// CHECK: warning: use of invalid pointer 'old'
// CHECK: warning: use of invalid pointer 'absent'
// CHECK: warning: use of invalid pointer 'absent'
// CHECK-NOT: warning:
