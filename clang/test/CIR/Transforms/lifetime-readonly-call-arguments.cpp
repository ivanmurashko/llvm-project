// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -std=c++17 -fclangir -fclangir-lifetime-check -verify -emit-cir %s -o /dev/null
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -std=c++17 -fclangir -fclangir-lifetime-check -clangir-disable-passes -emit-cir %s -o %t.cir
// RUN: cir-opt %t.cir --cir-canonicalize --cir-lifetime-check --verify-diagnostics -o /dev/null
// RUN: cir-opt %t.cir --emit-bytecode -o %t.bc
// RUN: cir-opt %t.bc --cir-lifetime-check --verify-diagnostics -o /dev/null
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -std=c++14 -fclangir -fclangir-lifetime-check -verify -emit-cir %s -o /dev/null
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -std=c++14 -fclangir -fclangir-lifetime-check -clangir-disable-passes -emit-cir %s -o %t.cxx14.cir
// RUN: cir-opt %t.cxx14.cir --cir-canonicalize --cir-lifetime-check --verify-diagnostics -o /dev/null
// RUN: cir-opt %t.cxx14.cir --emit-bytecode -o %t.cxx14.bc
// RUN: cir-opt %t.cxx14.bc --cir-lifetime-check --verify-diagnostics -o /dev/null
// expected-no-diagnostics

struct Input {
  int a, b, c, d, e, f, g, h, i, j, k, l, m;
};
void sink(int, int, int, int, int, int, int, int, int, int, int, int,
          int) noexcept;

// Member address calculations and loads provide initialized scalar arguments
// to the following call.
void read_fields(const Input &input) {
  sink(input.a, input.b, input.c, input.d, input.e, input.f, input.g,
       input.h, input.i, input.j, input.k, input.l, input.m);
}

struct Refs { const int *first; };
Refs tie(const int &, const int &, const int &, const int &, const int &,
         const int &) noexcept;
bool operator==(const Refs &, const Refs &) noexcept;

#if __cplusplus >= 201703L
// Reduced from AvailabilityInfo's six-field tie comparison. Exercise nested
// record-valued calls and the lifetime of their argument temporaries.
bool compare_fields(const Input &left, const Input &right) {
  return tie(left.a, left.b, left.c, left.d, left.e, left.f) ==
         tie(right.a, right.b, right.c, right.d, right.e, right.f);
}
#endif
