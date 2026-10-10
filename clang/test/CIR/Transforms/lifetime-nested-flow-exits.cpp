// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -std=c++20 -fexceptions -fcxx-exceptions -fclangir -fclangir-lifetime-check="history=all" -verify -emit-cir %s -o /dev/null
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -std=c++20 -fexceptions -fcxx-exceptions -fclangir -fclangir-lifetime-check -clangir-disable-passes -emit-cir %s -o %t.cir
// RUN: cir-opt %t.cir --cir-canonicalize --cir-lifetime-check="history=all" -o /dev/null 2>&1 | FileCheck %s
// RUN: cir-opt %t.cir --emit-bytecode -o %t.bc
// RUN: cir-opt %t.bc --cir-canonicalize --cir-lifetime-check="history=all" -o /dev/null 2>&1 | FileCheck %s

// These scalar examples exercise retained CIR move policy. The loop target
// and cleanup behavior must not invent or discard a possible later use.
void consume(int &&) noexcept;
void observe(int) noexcept;

void inner_break_reaches_outer_increment(bool condition) {
  int value = 1;
  for (; condition; consume(static_cast<int &&>(value))) { // expected-note {{moved here via std::move or rvalue reference}}
    for (;;) {
      break;
    }
    observe(value); // expected-warning {{use of moved-from value 'value'}}
    value = 2;
  }
}

void inner_continue_runs_inner_increment(bool outer, bool inner) {
  int value = 1;
  while (outer) {
    for (; inner; consume(static_cast<int &&>(value))) { // expected-note {{moved here via std::move or rvalue reference}}
      observe(value); // expected-warning {{use of moved-from value 'value'}}
      value = 2;
      continue;
    }
    break;
  }
}

void inner_continue_does_not_skip_outer_reset(bool condition) {
  int value = 1;
  for (;;) {
    consume(static_cast<int &&>(value));
    for (; condition;) {
      continue;
    }
    value = 2;
    break;
  }
  observe(value);
}

void outer_break_skips_outer_increment() {
  int value = 1;
  for (;; consume(static_cast<int &&>(value))) {
    for (;;) {
      break;
    }
    break;
  }
  observe(value);
}

struct Stop {
  Stop() noexcept;
  [[noreturn]] ~Stop() noexcept;
};
struct ReturnNormally {
  ReturnNormally() noexcept;
  ~ReturnNormally() noexcept;
};

void noreturn_destructor_prevents_another_iteration(bool condition) {
  int value = 1;
  while (condition) {
    observe(value);
    consume(static_cast<int &&>(value));
    Stop{};
  }
}

void normal_destructor_allows_another_iteration(bool condition) {
  int value = 1;
  while (condition) {
    observe(value); // expected-warning {{use of moved-from value 'value'}}
    value = 2;
    consume(static_cast<int &&>(value)); // expected-note {{moved here via std::move or rvalue reference}}
    ReturnNormally{};
  }
}

struct ThrowInsteadOfReturning {
  ThrowInsteadOfReturning() noexcept;
  [[noreturn]] ~ThrowInsteadOfReturning() noexcept(false);
};

void noreturn_destructor_preserves_exceptional_exit() {
  int value = 1;
  try {
    consume(static_cast<int &&>(value)); // expected-note {{moved here via std::move or rvalue reference}}
    ThrowInsteadOfReturning{};
  } catch (...) {
    observe(value); // expected-warning {{use of moved-from value 'value'}}
  }
}

// CHECK: warning: use of moved-from value 'value'
// CHECK: warning: use of moved-from value 'value'
// CHECK: warning: use of moved-from value 'value'
// CHECK: warning: use of moved-from value 'value'
// CHECK-NOT: warning:
// CHECK-NOT: error:
