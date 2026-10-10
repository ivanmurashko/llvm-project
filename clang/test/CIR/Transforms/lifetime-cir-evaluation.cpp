// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -std=c++14 -fclangir -fclangir-lifetime-check="history=all" -verify -emit-cir %s -o /dev/null
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -std=c++17 -fclangir -fclangir-lifetime-check="history=all" -verify -emit-cir %s -o /dev/null
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -std=c++17 -fclangir -fclangir-lifetime-check -clangir-disable-passes -emit-cir %s -o %t.cir
// RUN: cir-opt %t.cir --cir-canonicalize --cir-lifetime-check="history=all" --verify-diagnostics -o /dev/null
// RUN: cir-opt %t.cir --emit-bytecode -o %t.bc
// RUN: cir-opt %t.bc --cir-lifetime-check="history=all" --verify-diagnostics -o /dev/null

// These expressions have explicit sequencing reflected in their CIR. They
// do not require exploring alternative C++ argument evaluation orders.
void consume(int &&) noexcept;
int observe(int) noexcept;

void comma_and_short_circuit_reset() {
  int value = 0;
  consume(static_cast<int &&>(value));
  (value = 1, observe(value));
  consume(static_cast<int &&>(value));
  bool result = (value = 1, true) && observe(value);
}

void skipped_short_circuit() {
  int value = 0;
  bool left = false && (consume(static_cast<int &&>(value)), true);
  bool right = true || (consume(static_cast<int &&>(value)), false);
  observe(value);
}

void ordered_braced_initialization() {
  int value = 0;
  int safe[] = {(consume(static_cast<int &&>(value)), 0), (value = 1),
                observe(value)};
  int diagnosed[] = {(consume(static_cast<int &&>(value)), 0), // expected-note {{moved here via std::move or rvalue reference}}
                     observe(value), // expected-warning {{use of moved-from value 'value'}}
                     (value = 1)};
  observe(value);
}

struct Temporary {
  Temporary() noexcept;
  ~Temporary() noexcept;
};

void temporary_and_reset() {
  int value = 0;
  (consume(static_cast<int &&>(value)), Temporary(), value = 1,
   observe(value));
}

void invalid_use_before_temporary_cleanup() {
  int value = 0;
  (consume(static_cast<int &&>(value)), Temporary(), // expected-note {{moved here via std::move or rvalue reference}}
   observe(value)); // expected-warning {{use of moved-from value 'value'}}
}
