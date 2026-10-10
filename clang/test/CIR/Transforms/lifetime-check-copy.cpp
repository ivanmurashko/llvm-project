// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -std=c++20 -mconstructor-aliases -fclangir -fclangir-lifetime-check="history=all" -verify -emit-cir %s -o %t.cir
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -std=c++20 -mconstructor-aliases -fclangir -clangir-disable-passes -emit-cir %s -o %t.raw.cir
// RUN: cir-opt %t.raw.cir --cir-canonicalize --cir-lifetime-check="history=all" -o /dev/null 2>&1 | FileCheck %s

struct [[gsl::Owner(int)]] Owner {
  Owner();
  Owner(const Owner &);
};

struct [[gsl::Pointer(int)]] Pointer {
  Pointer();
  Pointer(const Pointer &);
  Pointer(const Owner &);
  Pointer &operator=(const Pointer &);
  int &operator*() const;
};

void copyOwnerPreservesSource() {
  Owner original;
  Pointer view(original);
  {
    Owner copy(original);
  }
  *view = 1;
}

void copyPointerPreservesPointee() {
  Pointer escaped;
  {
    Owner owner;
    Pointer source(owner);
    Pointer copy(source);
    escaped = copy;
  } // expected-note {{pointee 'owner' invalidated at end of scope}}
  *escaped = 1; // expected-warning {{use of invalid pointer 'escaped'}}
  // CHECK: warning: use of invalid pointer 'escaped'
  // CHECK: note: pointee 'owner' invalidated at end of scope
}

void copyNullPointerPreservesHistory() {
  Pointer source; // expected-note {{invalidated here}}
  Pointer copy(source);
  *copy = 1; // expected-warning {{use of invalid pointer 'copy'}}
  // CHECK: warning: use of invalid pointer 'copy'
  // CHECK: note: 'nullptr' invalidated here
}
