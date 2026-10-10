// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -std=c++23 -mconstructor-aliases -fclangir -fclangir-lifetime-check="history=all" -verify -emit-cir %s -o /dev/null
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -std=c++23 -mconstructor-aliases -fclangir -fclangir-lifetime-check -clangir-disable-passes -emit-cir %s -o %t.cir
// RUN: cir-opt %t.cir --cir-canonicalize --cir-lifetime-check="history=all" -o /dev/null 2>&1 | FileCheck %s
// RUN: cir-opt %t.cir --emit-bytecode -o %t.bc
// RUN: cir-opt %t.bc --cir-canonicalize --cir-lifetime-check="history=all" -o /dev/null 2>&1 | FileCheck %s

namespace std {
template <class T> T &&move(T &value) { return static_cast<T &&>(value); }
template <class T> T &&forward(T &value) { return static_cast<T &&>(value); }
template <class T> class [[gsl::Owner(T)]] unique_ptr {
public:
  explicit unique_ptr(T *) noexcept;
  unique_ptr(unique_ptr &&) noexcept;
  ~unique_ptr() noexcept;
  void reset(T * = nullptr) noexcept;
  T *get() const noexcept;
  T &operator*() const noexcept;
};
} // namespace std

int moving_through_reference_alias() {
  std::unique_ptr<int> source(new int(1));
  auto &alias = source;
  std::unique_ptr<int> destination(std::move(alias)); // expected-note {{moved here via std::move or rvalue reference}}
  return *source; // expected-warning {{use of invalid pointer 'source'}}
}

int resetting_through_reference_alias() {
  std::unique_ptr<int> source(new int(1));
  auto &alias = source;
  std::unique_ptr<int> destination(std::move(alias));
  alias.reset(new int(2));
  return *source;
}

int forwarding_owner_parameter(std::unique_ptr<int> &&source) {
  std::unique_ptr<int> destination(std::forward<std::unique_ptr<int>>(source)); // expected-note {{moved here via std::move or rvalue reference}}
  return *source; // expected-warning {{use of invalid pointer 'source'}}
}

int resetting_forwarded_parameter(std::unique_ptr<int> &&source) {
  std::unique_ptr<int> destination(std::forward<std::unique_ptr<int>>(source));
  source.reset(new int(2));
  return *source;
}

int moving_into_local_capture() {
  std::unique_ptr<int> source(new int(1));
  auto closure = [captured = std::move(source)] {}; // expected-note {{moved here via std::move or rvalue reference}}
  return *source; // expected-warning {{use of invalid pointer 'source'}}
}

int resetting_after_local_capture() {
  std::unique_ptr<int> source(new int(1));
  auto closure = [captured = std::move(source)] {};
  source.reset(new int(2));
  return *source;
}

int local_reference_capture_does_not_move() {
  std::unique_ptr<int> source(new int(1));
  auto closure = [&source] {};
  return *source;
}

int live_owner_capture_keeps_borrow() {
  std::unique_ptr<int> source(new int(1));
  int *borrowed = source.get();
  auto closure = [captured = std::move(source)] {};
  source.reset(new int(2));
  return *borrowed;
}

// CHECK: warning: use of invalid pointer 'source'
// CHECK: warning: use of invalid pointer 'source'
// CHECK: warning: use of invalid pointer 'source'
// CHECK-NOT: warning:
// CHECK-NOT: error:
