// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -std=c++20 -fclangir -fclangir-lifetime-check="history=all" -verify -emit-cir %s -o %t.cir
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -std=c++20 -fclangir -fclangir-lifetime-check -clangir-disable-passes -emit-cir %s -o %t.raw.cir
// RUN: cir-opt %t.raw.cir --cir-canonicalize --cir-lifetime-check="history=all" -o /dev/null 2>&1 | FileCheck %s
// RUN: cir-opt %t.raw.cir --emit-bytecode -o %t.bc
// RUN: cir-opt %t.bc --cir-lifetime-check="history=all" -o /dev/null 2>&1 | FileCheck %s

namespace std {
template <class A, class B> struct tuple {
  tuple(A, B) noexcept;
  template <class C, class D> tuple &operator=(const tuple<C, D> &) noexcept;
};
template <class A, class B> tuple<A &, B &> tie(A &, B &) noexcept;
template <class A, class B> tuple<A, B> make_tuple(A, B) noexcept;
}

int value_tuple_assignment_replaces_payload() {
  int value = 1, ignored = 0;
  int *destination = &value;
  std::tuple<int *, int> source(destination, 0);
  int *empty = nullptr; // expected-note {{'nullptr' invalidated here}}
  source = std::make_tuple(empty, 2);
  std::tie(destination, ignored) = source;
  return *destination; // expected-warning {{use of invalid pointer 'destination'}}
}

int value_tuple_assignment_restores_payload() {
  int value = 1, ignored = 0;
  int *destination = nullptr;
  std::tuple<int *, int> source(destination, 0);
  source = std::make_tuple(&value, 2);
  std::tie(destination, ignored) = source;
  return *destination;
}

int tuple_snapshot_tracks_pointee_lifetime() {
  int *destination = nullptr, ignored = 0;
  std::tuple<int *, int> source(destination, 0);
  {
    int local = 1;
    source = std::make_tuple(&local, 2);
  } // expected-note {{pointee 'local' invalidated at end of scope}}
  std::tie(destination, ignored) = source;
  return *destination; // expected-warning {{use of invalid pointer 'destination'}}
}

// CHECK: warning: use of invalid pointer 'destination'
// CHECK: warning: use of invalid pointer 'destination'
// CHECK-NOT: warning:
