// RUN: %clang_cc1 -std=c++20 -triple x86_64-unknown-linux-gnu -fclangir -clangir-disable-passes -emit-cir %s -o - | FileCheck %s --check-prefix=CIR
// RUN: %clang_cc1 -std=c++20 -triple x86_64-unknown-linux-gnu -fclangir -fclangir-lifetime-check="history=all" -verify -emit-cir %s -o /dev/null

struct [[gsl::Owner]] Owner {
  int value;
  void invalidate();
};

struct [[gsl::Pointer]] Condition {
  int *value;
  Condition(const Owner &);
  explicit operator bool() const;
  ~Condition(); // expected-warning {{use of invalid pointer 'condition'}}
};

// A condition variable is destroyed after the step, so the step invalidates
// the resource before its use by Condition's destructor. CIR represents this
// with separate condition, body, step, and cleanup regions.
//
// The bounded loop model omits the step from its first path, retaining cleanup.
// If it instead drops the last region (cleanup), that first path executes the
// invalidating step and restores condition's initial invalid pointer set. The
// unchanged-state shortcut then skips the subsequent path and misses the
// destructor warning. Selecting the step explicitly exposes the warning on the
// subsequent path without requiring a stronger loop approximation.
extern "C" void loop_step_before_cleanup() {
  Owner owner;
  for (; Condition condition{owner}; owner.invalidate()) { // expected-note {{invalidated by non-const use of owner type}}
  }
}

// CIR-LABEL: cir.func {{.*}}@loop_step_before_cleanup(
// CIR: %[[COND:.*]] = cir.alloca "condition"
// CIR: cir.for : cond {
// CIR: cir.call @_ZN9ConditionC1ERK5Owner(%[[COND]],
// CIR: cir.call @_ZNK9ConditioncvbEv(%[[COND]])
// CIR: } body {
// CIR: } step {
// CIR: cir.call @_ZN5Owner10invalidateEv
// CIR: } cleanup normal {
// CIR: cir.if
// CIR: cir.call @_ZN9ConditionD1Ev(%[[COND]])
