// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -std=c++17 -fclangir -fclangir-lifetime-check="history=all" -verify -emit-cir %s -o /dev/null
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -std=c++17 -fclangir -fclangir-lifetime-check -clangir-disable-passes -emit-cir %s -o %t.cir
// RUN: cir-opt %t.cir --cir-canonicalize --cir-lifetime-check="history=all" -o /dev/null 2>&1 | FileCheck %s
// RUN: cir-opt %t.cir --emit-bytecode -o %t.bc
// RUN: cir-opt %t.bc --cir-lifetime-check="history=all" -o /dev/null 2>&1 | FileCheck %s

// A by-value pointer-like record is an initialized object even though its
// incoming record SSA value is not an address. This mirrors StringRef and
// string_view parameters without depending on a particular standard library.
struct [[gsl::Pointer(int)]] View {
  int *data;
  unsigned length;
  unsigned size() const noexcept;
};
void consume(View &&) noexcept;

unsigned use_parameter(View value) { return value.size(); }

unsigned copy_parameter(View value) {
  View copy = value;
  return copy.size();
}

unsigned assign_parameter(View value) {
  View copy;
  copy = value;
  return copy.size();
}

// An unmodeled external object's contents are unknown, not an uninitialized
// local value and not proof of non-nullness.
extern View external;
unsigned copy_external_record() {
  View copy = external;
  return copy.size();
}

unsigned use_moved_parameter(View value) {
  consume(static_cast<View &&>(value)); // expected-note {{moved here via std::move or rvalue reference}}
  return value.size(); // expected-warning {{use of invalid pointer 'value'}}
}

unsigned copy_moved_parameter(View value) {
  consume(static_cast<View &&>(value)); // expected-note 2 {{moved here via std::move or rvalue reference}}
  View copy = value; // expected-warning {{use of invalid pointer 'value'}}
  return copy.size(); // expected-warning {{use of invalid pointer 'copy'}}
}

unsigned copy_uninitialized_record() {
  View value; // expected-note 2 {{uninitialized here}}
  View copy = value; // expected-warning {{use of invalid pointer 'value'}}
  return copy.size(); // expected-warning {{use of invalid pointer 'copy'}}
}

int uninitialized_raw_pointer() {
  int *pointer; // expected-note {{uninitialized here}}
  return *pointer; // expected-warning {{use of invalid pointer 'pointer'}}
}

struct Plain { int member; };
void consume(Plain &&) noexcept;

int copy_moved_whole_record() {
  Plain value{1};
  consume(static_cast<Plain &&>(value)); // expected-note {{moved here via std::move or rvalue reference}}
  Plain copy = value; // expected-warning {{use of moved-from value 'value'}}
  return copy.member;
}

int copy_initialized_whole_record(Plain value) {
  Plain copy = value;
  return copy.member;
}

// CHECK: warning: use of invalid pointer 'value'
// CHECK: warning: use of invalid pointer 'value'
// CHECK: warning: use of invalid pointer 'copy'
// CHECK: warning: use of invalid pointer 'value'
// CHECK: warning: use of invalid pointer 'copy'
// CHECK: warning: use of invalid pointer 'pointer'
// CHECK: warning: use of moved-from value 'value'
// CHECK-NOT: warning:
// CHECK-NOT: error:
