// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -std=c++17 -fexceptions -fcxx-exceptions -fclangir -fclangir-lifetime-check="history=all" -verify -emit-cir %s -o /dev/null
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -std=c++17 -fexceptions -fcxx-exceptions -fclangir -fclangir-lifetime-check -clangir-disable-passes -emit-cir %s -o %t.cir
// RUN: cir-opt %t.cir --cir-canonicalize --cir-lifetime-check="history=all" --verify-diagnostics -o /dev/null
// RUN: cir-opt %t.cir --emit-bytecode -o %t.bc
// RUN: cir-opt %t.bc --cir-lifetime-check="history=all" --verify-diagnostics -o /dev/null

struct Temporary {
  Temporary() noexcept;
  ~Temporary() noexcept;
};
struct ThrowingTemporary {
  ThrowingTemporary();
  ~ThrowingTemporary() noexcept;
};
void consume(int &&) noexcept;
void observe(int) noexcept;

// Construction and cleanup follow the CIR regions. A throwing constructor
// cannot reach the following reset, and earlier completed objects unwind.
void constructor_throws_before_reset() {
  int value = 0;
  try {
    Temporary first;
    consume(static_cast<int &&>(value)); // expected-note {{moved here via std::move or rvalue reference}}
    ThrowingTemporary second;
    value = 1;
  } catch (...) {
    observe(value); // expected-warning {{use of moved-from value 'value'}}
  }
}

void reset_before_throwing_constructor() {
  int value = 0;
  try {
    Temporary first;
    consume(static_cast<int &&>(value));
    value = 1;
    ThrowingTemporary second;
  } catch (...) {
    observe(value);
  }
}
