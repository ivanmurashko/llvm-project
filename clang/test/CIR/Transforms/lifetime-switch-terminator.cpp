// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -std=c++20 -fclangir -fclangir-lifetime-check="history=all" -emit-cir %s -o /dev/null -verify
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -std=c++20 -fclangir -clangir-disable-passes -emit-cir %s -o - | FileCheck %s

// A source statement after return becomes a predecessorless block. It must
// not turn a terminating case into a fallthrough case.
void return_then_dead_break(int n) {
  int value = 0;
  int *p = &value;
  switch (n) {
  case 0:
    p = nullptr;
    return;
    break;
  case 1:
    *p = 1;
    break;
  default: break;
  }
}

// CHECK-LABEL: cir.func {{.*}} @_Z22return_then_dead_breaki(
// CHECK: cir.return
// CHECK: ^bb{{[0-9]+}}:  // no predecessors
// CHECK-NEXT: cir.break

// A lexical scope can contain a dead yield after its real return terminator.
void scoped_return(int n) {
  int value = 0;
  int *p = &value;
  switch (n) {
  case 0: {
    p = nullptr;
    return;
  }
  case 1:
    *p = 1;
    break;
  default: break;
  }
}

// CHECK-LABEL: cir.func {{.*}} @_Z13scoped_returni(
// CHECK: cir.scope
// CHECK: cir.return

// The same exit handling is required for a break inside a lexical scope.
void scoped_break(int n) {
  int value = 0;
  int *p = &value;
  switch (n) {
  case 0: {
    p = nullptr;
    break;
  }
  case 1:
    *p = 1;
    break;
  default: break;
  }
}

// CHECK-LABEL: cir.func {{.*}} @_Z12scoped_breaki(
// CHECK: cir.scope
// CHECK: cir.break

// Both paths leave the switch. The synthetic continuation of the nested break
// must not propagate a null pointer into case 1.
void conditional_scoped_break(int n, bool stop) {
  int value = 0;
  int *p = &value;
  switch (n) {
  case 0: {
    if (stop) {
      p = nullptr;
      break;
    }
    p = nullptr;
    break;
  }
  case 1:
    *p = 1;
    break;
  default: break;
  }
}

// CHECK-LABEL: cir.func {{.*}} @_Z24conditional_scoped_breakib(
// CHECK: cir.if
// CHECK: cir.break

struct Cleanup {
  ~Cleanup();
};

// Normal destruction leaves a synthetic yield block after the break in the
// cleanup body. Its presence does not create a path into the next case.
void cleanup_scoped_break(int n, bool stop) {
  int value = 0;
  int *p = &value;
  switch (n) {
  case 0: {
    Cleanup cleanup;
    if (stop) {
      p = nullptr;
      break;
    }
    p = nullptr;
    break;
  }
  case 1:
    *p = 1;
    break;
  default: break;
  }
}

// CHECK-LABEL: cir.func {{.*}} @_Z20cleanup_scoped_breakib(
// CHECK: cir.cleanup.scope
// CHECK: cir.break
// CHECK: ^bb{{[0-9]+}}:  // no predecessors
// CHECK-NEXT: cir.yield

// Breaks in nested loops and switches do not exit the enclosing case. Keep
// its fallthrough edge so the next case observes the null assignment.
void nested_breaks(int n, int inner, bool repeat) {
  int value = 0;
  int *p = &value;
  switch (n) {
  case 0:
    while (repeat) {
      break;
    }
    switch (inner) {
    case 0: break;
    default: break;
    }
    p = nullptr; // expected-note {{invalidated here}}
    [[fallthrough]];
  case 1:
    *p = 1; // expected-warning {{use of invalid pointer 'p'}}
    break;
  default: break;
  }
}

// CHECK-LABEL: cir.func {{.*}} @_Z13nested_breaksiib(
// CHECK: cir.while
// CHECK: cir.break
// CHECK: cir.switch
// CHECK: cir.break
