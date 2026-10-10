// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -std=c++20 -fclangir -fclangir-lifetime-check="history=all" -verify -emit-cir %s -o /dev/null

struct Inner {
  int *p;
};
struct Outer {
  Inner inner;
  int *other;
};

// Nested pointer fields already tracked by the analysis must have printable
// diagnostic names. This does not extend compound move tracking.
void nested_uninitialized() {
  Outer outer; // expected-note {{uninitialized here}}
  *outer.inner.p = 1; // expected-warning {{use of invalid pointer 'outer.inner.p'}}
}
