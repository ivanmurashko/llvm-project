// RUN: %clang_cc1 -std=c++20 -triple x86_64-unknown-linux-gnu -fclangir -clangir-disable-passes -emit-cir %s -o - | FileCheck %s --check-prefix=CIR
// RUN: %clang_cc1 -std=c++20 -triple x86_64-unknown-linux-gnu -fclangir -fclangir-lifetime-check="history=all" -verify -emit-cir %s -o /dev/null

struct Guard {
  ~Guard();
};

// A non-trivial destructor puts the following statements in the body of a
// cleanup scope. The analysis must visit that body, not just ordinary scopes.
extern "C" void cleanup_scope_body() {
  Guard guard;
  int *p = nullptr; // expected-note {{invalidated here}}
  *p = 1;          // expected-warning {{use of invalid pointer 'p'}}
}

// CIR-LABEL: cir.func {{.*}}@cleanup_scope_body(
// CIR: cir.cleanup.scope {
// CIR: cir.const #cir.ptr<null>
// CIR: cir.load deref
// CIR: } cleanup normal {
// CIR: cir.call @_ZN5GuardD1Ev

struct [[gsl::Owner]] Owner {
  int value;
  void invalidate();
};

// A pointer-category object can use its borrowed resource during destruction.
// Its implicit destructor call must be checked in the normal cleanup region.
struct [[gsl::Pointer]] Borrower {
  int *value;
  Borrower(const Owner &);
  ~Borrower(); // expected-warning {{use of invalid pointer 'borrow'}}
};

extern "C" void cleanup_scope_invalid_borrow() {
  Owner owner;
  Borrower borrow(owner);
  owner.invalidate(); // expected-note {{invalidated by non-const use of owner type}}
}

// CIR-LABEL: cir.func {{.*}}@cleanup_scope_invalid_borrow(
// CIR: %[[BORROW:.*]] = cir.alloca "borrow"
// CIR: cir.call @_ZN8BorrowerC1ERK5Owner(%[[BORROW]],
// CIR: cir.cleanup.scope {
// CIR: cir.call @_ZN5Owner10invalidateEv
// CIR: } cleanup normal {
// CIR: cir.call @_ZN8BorrowerD1Ev(%[[BORROW]])

// Destruction while the owner is still valid must not introduce a warning.
extern "C" void cleanup_scope_valid_borrow() {
  Owner owner;
  Borrower valid(owner);
}

// CIR-LABEL: cir.func {{.*}}@cleanup_scope_valid_borrow(
// CIR: cir.cleanup.scope {
// CIR: } cleanup normal {
// CIR: cir.call @_ZN8BorrowerD1Ev
