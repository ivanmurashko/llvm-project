// RUN: %clang_cc1 -std=c++20 -triple x86_64-unknown-linux-gnu -I%S/Inputs -mconstructor-aliases -fclangir -fclangir-lifetime-check="history=all;remarks=all" -Rremark-clangir -verify -emit-cir %s -o %t.cir

#include "std-cxx.h"

struct S {
  S(int, int, const S* s);
  void f(int a, int b);
};

void S::f(int a, int b) {
  std::shared_ptr<S> l = std::make_shared<S>(a, b, this); // expected-remark 2 {{pset => { fn_arg:0 }}}
}
