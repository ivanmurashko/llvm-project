// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -std=c++20 -fclangir -fclangir-lifetime-check="history=all" -emit-cir %s -o /dev/null -verify
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -std=c++20 -fclangir -emit-cir %s -o - | FileCheck %s

// A label splits a case into multiple CIR blocks even without a goto. Keep
// checking its body and use the final terminator to distinguish fallthrough
// from break, as for a single-block case.
void fallthrough(int n) {
  int value = 0;
  int *p = &value;
  switch (n) {
  case 0:
  label_fallthrough:
    p = nullptr; // expected-note {{invalidated here}}
    [[fallthrough]];
  case 1:
    *p = 1; // expected-warning {{use of invalid pointer 'p'}}
    break;
  default: break;
  }
}

// CHECK-LABEL: cir.func {{.*}} @_Z11fallthroughi(
// CHECK: cir.case(equal, [#cir.int<0>
// CHECK: cir.br ^[[FALLTHROUGH:bb[0-9]+]]
// CHECK: ^[[FALLTHROUGH]]:
// CHECK: cir.label "label_fallthrough"
// CHECK: cir.yield
// CHECK: cir.case(equal, [#cir.int<1>

void breaks(int n) {
  int value = 0;
  int *p = &value;
  switch (n) {
  case 0:
  label_break:
    p = nullptr; // expected-note {{invalidated here}}
    break;
  case 1:
    *p = 1; // This case has no fallthrough predecessor; p remains valid here.
    break;
  default: break;
  }
  *p = 2; // expected-warning {{use of invalid pointer 'p'}}
}

// CHECK-LABEL: cir.func {{.*}} @_Z6breaksi(
// CHECK: cir.case(equal, [#cir.int<0>
// CHECK: cir.br ^[[BREAK:bb[0-9]+]]
// CHECK: ^[[BREAK]]:
// CHECK: cir.label "label_break"
// CHECK: cir.break
// CHECK: cir.case(equal, [#cir.int<1>

void scope(int n) {
  int *p = nullptr;
  switch (n) {
  case 0:
    {
      int local = 0;
      p = &local;
    } // expected-note {{pointee 'local' invalidated at end of scope}}
  label_scope:
    *p = 1; // expected-warning {{use of invalid pointer 'p'}}
    break;
  default: break;
  }
}

// CHECK-LABEL: cir.func {{.*}} @_Z5scopei(
// CHECK: cir.case(equal, [#cir.int<0>
// CHECK: cir.scope
// CHECK: cir.br ^[[SCOPE:bb[0-9]+]]
// CHECK: ^[[SCOPE]]:
// CHECK: cir.label "label_scope"
