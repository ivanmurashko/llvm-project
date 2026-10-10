// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -std=c++20 -fclangir -fclangir-lifetime-check="history=all" -emit-cir %s -o /dev/null -verify
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -std=c++20 -fclangir -emit-cir %s -o - | FileCheck %s

// When no case matches, the pointer retains its incoming null state.
void no_match(int n) {
  int value = 0;
  int *p = nullptr; // expected-note {{invalidated here}}
  switch (n) {
  case 0:
    p = &value;
    break;
  }
  *p = 1; // expected-warning {{use of invalid pointer 'p'}}
}

// CHECK-LABEL: cir.func {{.*}} @_Z8no_matchi(
// CHECK: cir.switch
// CHECK: cir.case(equal,
// CHECK-NOT: cir.case(default,

// A default covers the remaining inputs. Do not retain the incoming null state
// when every case replaces it with a valid pointer.
void with_default(int n) {
  int value = 0;
  int *p = nullptr;
  switch (n) {
  case 0:
    p = &value;
    break;
  default:
    p = &value;
    break;
  }
  *p = 1;
}

// CHECK-LABEL: cir.func {{.*}} @_Z12with_defaulti(
// CHECK: cir.switch
// CHECK: cir.case(equal,
// CHECK: cir.case(default,
