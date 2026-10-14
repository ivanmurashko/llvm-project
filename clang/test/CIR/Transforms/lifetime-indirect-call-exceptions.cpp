// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -std=c++20 -fexceptions -fcxx-exceptions -fclangir -fclangir-lifetime-check="history=all" -verify -emit-cir %s -o /dev/null
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -std=c++20 -fexceptions -fcxx-exceptions -fclangir -fclangir-lifetime-check -clangir-disable-passes -emit-cir %s -o %t.cir
// RUN: cir-opt %t.cir --mlir-disable-threading=true --cir-lifetime-check="history=all" -o /dev/null 2> %t.serial
// RUN: FileCheck %s --check-prefix=CIR --input-file=%t.serial --implicit-check-not='warning:' --implicit-check-not='error:' --implicit-check-not='remark:' --implicit-check-not='note:'
// RUN: cir-opt %t.cir --mlir-disable-threading=false --cir-lifetime-check="history=all" -o /dev/null 2> %t.parallel
// RUN: diff %t.serial %t.parallel
// RUN: cir-opt %t.cir --cir-canonicalize --mlir-print-debuginfo -o %t.canonical.cir
// RUN: cir-opt %t.canonical.cir --mlir-disable-threading=true --cir-lifetime-check="history=all" -o /dev/null 2> %t.canonical.serial
// RUN: diff %t.serial %t.canonical.serial
// RUN: cir-opt %t.canonical.cir --mlir-disable-threading=false --cir-lifetime-check="history=all" -o /dev/null 2> %t.canonical.parallel
// RUN: diff %t.serial %t.canonical.parallel
// RUN: cir-opt %t.canonical.cir --emit-bytecode -o %t.bc
// RUN: cir-opt %t.bc --mlir-disable-threading=true --cir-lifetime-check="history=all" -o /dev/null 2> %t.bytecode.serial
// RUN: diff %t.serial %t.bytecode.serial
// RUN: cir-opt %t.bc --mlir-disable-threading=false --cir-lifetime-check="history=all" -o /dev/null 2> %t.bytecode.parallel
// RUN: diff %t.serial %t.bytecode.parallel

void observe(int) noexcept;

// The indirect call can consume before it throws. A normal-path assignment
// cannot repair the earlier exceptional state.
void exceptional_consumption(void (*callee)(int &&)) {
  int value = 1;
  try {
    callee(static_cast<int &&>(value)); // expected-note {{moved here}}
    value = 2;
  } catch (...) {
    observe(value); // expected-warning {{use of moved-from value 'value'}}
  }
}

void normal_consumption(void (*callee)(int &&)) {
  int value = 1;
  try {
    callee(static_cast<int &&>(value)); // expected-note {{moved here}}
    observe(value); // expected-warning {{use of moved-from value 'value'}}
  } catch (...) {
    value = 2;
  }
}

void nothrow_call_keeps_normal_move(void (*callee)(int &&) noexcept) {
  int value = 1;
  try {
    callee(static_cast<int &&>(value)); // expected-note {{moved here}}
  } catch (...) {
    value = 2;
  }
  observe(value); // expected-warning {{use of moved-from value 'value'}}
}

void handler_reinitializes(void (*callee)(int &&)) {
  int value = 1;
  try {
    callee(static_cast<int &&>(value));
    value = 2;
  } catch (...) {
    value = 3;
  }
  observe(value);
}

// CIR: {{^.*}}lifetime-indirect-call-exceptions.cpp:28:{{[0-9]+}}: warning: use of moved-from value 'value'
// CIR: {{^.*}}lifetime-indirect-call-exceptions.cpp:25:{{[0-9]+}}: note: moved here
// CIR: {{^.*}}lifetime-indirect-call-exceptions.cpp:36:{{[0-9]+}}: warning: use of moved-from value 'value'
// CIR: {{^.*}}lifetime-indirect-call-exceptions.cpp:35:{{[0-9]+}}: note: moved here
// CIR: {{^.*}}lifetime-indirect-call-exceptions.cpp:49:{{[0-9]+}}: warning: use of moved-from value 'value'
// CIR: {{^.*}}lifetime-indirect-call-exceptions.cpp:45:{{[0-9]+}}: note: moved here
