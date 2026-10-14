// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -std=c++20 -fclangir -fclangir-lifetime-check="history=all" -verify -emit-cir %s -o /dev/null
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -std=c++20 -fclangir -fclangir-lifetime-check -clangir-disable-passes -emit-cir %s -o %t.cir
// RUN: cir-opt %t.cir --cir-canonicalize --mlir-disable-threading=true --cir-lifetime-check="history=all" -o /dev/null 2> %t.serial
// RUN: FileCheck %s --input-file=%t.serial --implicit-check-not='warning:' --implicit-check-not='error:' --implicit-check-not='remark:' --implicit-check-not='note:'
// RUN: cir-opt %t.cir --cir-canonicalize --mlir-disable-threading=false --cir-lifetime-check="history=all" -o /dev/null 2> %t.parallel
// RUN: diff %t.serial %t.parallel
// RUN: cir-opt %t.cir --cir-canonicalize --mlir-print-debuginfo -o %t.canonical.cir
// RUN: cir-opt %t.canonical.cir --mlir-disable-threading=true --cir-lifetime-check="history=all" -o /dev/null 2> %t.text
// RUN: diff %t.serial %t.text
// RUN: cir-opt %t.cir --cir-canonicalize --emit-bytecode -o %t.bc
// RUN: cir-opt %t.bc --mlir-disable-threading=true --cir-lifetime-check="history=all" -o /dev/null 2> %t.bytecode.serial
// RUN: diff %t.serial %t.bytecode.serial
// RUN: cir-opt %t.bc --mlir-disable-threading=false --cir-lifetime-check="history=all" -o /dev/null 2> %t.bytecode.parallel
// RUN: diff %t.serial %t.bytecode.parallel

// Previously the SKIP_STORE and BACKEDGE cases in lifetime-switch-unsupported
// failed before analysis. These are supported same/ancestor-region jumps in
// direct case regions. Both warnings require CIR control-flow propagation.
void skip_store(int n, bool skip) {
  int value = 0;
  int *p = nullptr; // expected-note {{invalidated here}}
  switch (n) {
  case 0:
    if (skip)
      goto use;
    p = &value;
  use:
    *p = 1; // expected-warning {{use of invalid pointer 'p'}}
    break;
  default:
    break;
  }
}
// CHECK: warning: use of invalid pointer 'p'
// CHECK: note: {{.*}}invalidated here

// The first visit is valid. Only a revisit after the null store warns, so
// following labels once in source order would miss the defect.
void backedge(int n, bool again) {
  int value = 0;
  int *p = &value;
  switch (n) {
  case 0:
  repeat:
    *p = 1; // expected-warning {{use of invalid pointer 'p'}}
    p = nullptr; // expected-note {{invalidated here}}
    if (again)
      goto repeat;
    break;
  default:
    break;
  }
}
// CHECK: warning: use of invalid pointer 'p'
// CHECK: note: {{.*}}invalidated here
