// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -std=c++17 -fexceptions -fcxx-exceptions -fclangir -fclangir-lifetime-check="history=all" -verify -emit-cir %s -o /dev/null
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -std=c++17 -fexceptions -fcxx-exceptions -fclangir -fclangir-lifetime-check -clangir-disable-passes -emit-cir %s -o %t.cir
// RUN: cir-opt %t.cir --cir-canonicalize --cir-lifetime-check="history=all" -o /dev/null 2>&1 | FileCheck %s --implicit-check-not=warning: --implicit-check-not=error:
// RUN: cir-opt %t.cir --emit-bytecode -o %t.bc
// RUN: cir-opt %t.bc --cir-lifetime-check="history=all" -o /dev/null 2>&1 | FileCheck %s --implicit-check-not=warning: --implicit-check-not=error:

struct Pair { const int *first, *second; };
Pair make();
void observe(int) noexcept;

// A whole-record store of an opaque call result overwrites the declaration's
// uninitialized member slots. Its pointer contents are unknown, not non-null.
int opaque_return() {
  Pair returned = make();
  return *returned.first;
}

int guarded_return() {
  auto guarded = make();
  if (!guarded.first)
    return 0;
  for (int index = 0; index < 4; ++index) {
    if (guarded.first[index])
      return guarded.first[index];
    if (guarded.second && guarded.second[index])
      return guarded.second[index];
  }
  return 0;
}

int uninitialized_member() {
  Pair uninitialized; // expected-note {{uninitialized here}}
  return *uninitialized.first; // expected-warning {{use of invalid pointer 'uninitialized.first'}}
}

int null_member() {
  Pair null_value{nullptr, nullptr}; // expected-note {{'nullptr' invalidated here}}
  return *null_value.first; // expected-warning {{use of invalid pointer 'null_value.first'}}
}

int dangling_member() {
  Pair dangling = make();
  {
    int local = 1;
    dangling.first = &local;
  } // expected-note {{pointee 'local' invalidated at end of scope}}
  return *dangling.first; // expected-warning {{use of invalid pointer 'dangling.first'}}
}

int mixed_unknown_null(bool choose) {
  Pair mixed = make();
  if (choose)
    mixed.first = nullptr; // expected-note {{'nullptr' invalidated here}}
  return *mixed.first; // expected-warning {{use of invalid pointer 'mixed.first'}}
}

int mixed_unknown_dangling(bool choose) {
  Pair maybe_dangling = make();
  if (choose) {
    int local = 1;
    maybe_dangling.first = &local;
  } // expected-note {{pointee 'local' invalidated at end of scope}}
  return *maybe_dangling.first; // expected-warning {{use of invalid pointer 'maybe_dangling.first'}}
}

// The same opaque return store can appear inside an exception region.
void exception_before_store() {
  try {
    Pair temporary = make();
    observe(*temporary.first);
  } catch (...) {
  }
}

// CHECK-NOT: warning:
// CHECK: warning: use of invalid pointer 'uninitialized.first'
// CHECK-NOT: warning:
// CHECK: warning: use of invalid pointer 'null_value.first'
// CHECK-NOT: warning:
// CHECK: warning: use of invalid pointer 'dangling.first'
// CHECK-NOT: warning:
// CHECK: warning: use of invalid pointer 'mixed.first'
// CHECK-NOT: warning:
// CHECK: warning: use of invalid pointer 'maybe_dangling.first'
// CHECK-NOT: warning:
// CHECK-NOT: error:
