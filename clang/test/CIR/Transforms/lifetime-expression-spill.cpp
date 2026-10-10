// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -std=c++17 -fclangir -fclangir-lifetime-check -verify -emit-cir %s -o /dev/null
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -std=c++17 -fclangir -fclangir-lifetime-check -clangir-disable-passes -emit-cir %s -o %t.cir
// RUN: cir-opt %t.cir --cir-canonicalize --cir-lifetime-check -o /dev/null 2>&1 | FileCheck %s --allow-empty
// RUN: cir-opt %t.cir --emit-bytecode -o %t.bc
// RUN: cir-opt %t.bc --cir-canonicalize --cir-lifetime-check -o /dev/null 2>&1 | FileCheck %s --allow-empty
// expected-no-diagnostics
// CHECK-NOT: warning:
// CHECK-NOT: error:

// The temporary bound to the second parameter introduces a full-expression
// scope. The returned reference spills out of that scope and must retain
// its initialized pointer value through the cleanup.
int &reference(int &, const int &);
int returned_reference() {
  int value = 1;
  int &borrowed = reference(value, 0);
  return borrowed;
}
