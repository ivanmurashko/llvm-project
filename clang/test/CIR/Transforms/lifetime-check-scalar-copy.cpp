// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -std=c++20 -fclangir -fclangir-lifetime-check="history=all" -verify -emit-cir %s -o %t.cir
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -std=c++20 -fclangir -fclangir-lifetime-check -clangir-disable-passes -emit-cir %s -o %t.raw.cir
// RUN: FileCheck %s --check-prefix=CIR < %t.raw.cir
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -std=c++20 -fclangir -clangir-disable-passes -emit-cir %s -o %t.ordinary.cir
// RUN: FileCheck %s --check-prefix=NO-METADATA < %t.ordinary.cir
// RUN: cir-opt %t.raw.cir --emit-bytecode -o %t.bc
// RUN: cir-opt %t.bc --verify-roundtrip --cir-canonicalize --cir-lifetime-check="history=all" -o /dev/null 2>&1 | FileCheck %s --check-prefix=DIAG --implicit-check-not="use of moved-from value 'copy_source'" --implicit-check-not="use of moved-from value 'assign_source'" --implicit-check-not="use of moved-from value 'cast_only_source'" --implicit-check-not="use of moved-from value 'move_only_source'" --implicit-check-not="use of moved-from value 'comparison_source'"

namespace std {
template <class T> T &&move(T &value) { return static_cast<T &&>(value); }
}

void observe(int);
void consume(int &&);

extern "C" {

// CIR-LABEL: cir.func{{.*}} @copy_value
// CIR-NOT: cir.load xvalue
void copy_value() {
  int copy_source = 1;
  int copied = copy_source;
  observe(copy_source);
}

// CIR-LABEL: cir.func{{.*}} @copy_assignment
// CIR-NOT: cir.load xvalue
void copy_assignment() {
  int assign_source = 1;
  int assigned = 0;
  assigned = assign_source;
  observe(assign_source);
}

// A cast or reference binding alone does not read/consume the scalar.
// CIR-LABEL: cir.func{{.*}} @cast_only
// CIR-NOT: cir.load xvalue
void cast_only() {
  int cast_only_source = 1;
  (void)static_cast<int &&>(cast_only_source);
  observe(cast_only_source);
}

// CIR-LABEL: cir.func{{.*}} @move_only
// CIR-NOT: cir.load xvalue
void move_only() {
  int move_only_source = 1;
  (void)std::move(move_only_source);
  int &&reference = std::move(move_only_source);
  observe(move_only_source);
}

// Reading an xvalue records metadata but does not by itself consume the source.
// The comparison's result is stored, not the source value.
// CIR-LABEL: cir.func{{.*}} @xvalue_comparison
// CIR: cir.load xvalue
void xvalue_comparison() {
  int comparison_source = 1;
  bool same = (std::move(comparison_source) == 0);
  observe(comparison_source);
}

// Preserve the check's explicit scalar move-intent policy. The language-level
// int operation is a copy, but explicitly reading an xvalue remains consuming
// for the existing lifetime check's initialization/assignment behavior.
// CIR-LABEL: cir.func{{.*}} @move_initialization
// CIR: cir.load xvalue
void move_initialization() {
  int init_source = 1;
  int moved = std::move(init_source); // expected-note {{moved here via std::move or rvalue reference}}
  observe(init_source); // expected-warning {{use of moved-from value 'init_source'}}
}

// CIR-LABEL: cir.func{{.*}} @cast_initialization
// CIR: cir.load xvalue
void cast_initialization() {
  int cast_source = 1;
  int moved = static_cast<int &&>(cast_source); // expected-note {{moved here via std::move or rvalue reference}}
  observe(cast_source); // expected-warning {{use of moved-from value 'cast_source'}}
}

// CIR-LABEL: cir.func{{.*}} @move_assignment
// CIR: cir.load xvalue
void move_assignment() {
  int assignment_move_source = 1;
  int moved = 0;
  moved = std::move(assignment_move_source); // expected-note {{moved here via std::move or rvalue reference}}
  observe(assignment_move_source); // expected-warning {{use of moved-from value 'assignment_move_source'}}
}

// Existing rvalue-reference parameter metadata still establishes consumption.
// CIR-LABEL: cir.func{{.*}} @consuming_call
void consuming_call() {
  int call_source = 1;
  consume(std::move(call_source)); // expected-note {{moved here via std::move or rvalue reference}}
  int copied = call_source; // expected-warning {{use of moved-from value 'call_source'}}
}
}

// DIAG: warning: use of moved-from value 'init_source'
// DIAG: note: moved here via std::move or rvalue reference
// DIAG: warning: use of moved-from value 'cast_source'
// DIAG: note: moved here via std::move or rvalue reference
// DIAG: warning: use of moved-from value 'assignment_move_source'
// DIAG: note: moved here via std::move or rvalue reference
// DIAG: warning: use of moved-from value 'call_source'
// DIAG: note: moved here via std::move or rvalue reference

// NO-METADATA: module
// NO-METADATA-NOT: cir.load xvalue
