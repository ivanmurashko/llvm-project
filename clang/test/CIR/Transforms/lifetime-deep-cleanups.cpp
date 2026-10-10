// REQUIRES: thread_support
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -std=c++17 -fclangir -fclangir-lifetime-check="history=all" -verify -emit-cir %s -o /dev/null
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -std=c++17 -fclangir -fclangir-lifetime-check -clangir-disable-passes -emit-cir %s -o %t.cir
// RUN: cir-opt %t.cir --cir-canonicalize --cir-lifetime-check="history=all" -o /dev/null 2>&1 | FileCheck %s

// Every temporary needs a cleanup at the end of the full expression. The
// resulting nested CIR regions must not exhaust an ordinary 8 MiB stack.
// Keep a live pointer fact through the entire expression and check it after
// all normal cleanup paths have completed.
struct Temporary {
  Temporary(int);
  ~Temporary();
};
struct Element {
  Element(Temporary);
  ~Element();
};
void consume(const Element (&)[1200]);

#define E1 Element(Temporary(0))
#define E2 E1, E1
#define E4 E2, E2
#define E8 E4, E4
#define E16 E8, E8
#define E32 E16, E16
#define E64 E32, E32
#define E128 E64, E64
#define E256 E128, E128
#define E512 E256, E256
#define E1024 E512, E512

int deep_cleanups() {
  int *pointer = nullptr; // expected-note {{'nullptr' invalidated here}}
  consume({E1024, E128, E32, E16});
  return *pointer; // expected-warning {{use of invalid pointer 'pointer'}}
}

// CHECK: warning: use of invalid pointer 'pointer'
// CHECK: note: 'nullptr' invalidated here
