// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -std=c++20 -fclangir -fclangir-lifetime-check="history=all" -verify -emit-cir %s -o /dev/null
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -std=c++20 -fclangir -fclangir-lifetime-check -clangir-disable-passes -emit-cir %s -o %t.cir
// RUN: cir-opt %t.cir --cir-canonicalize --mlir-disable-threading=true --cir-lifetime-check="history=all" -o /dev/null 2> %t.serial
// RUN: FileCheck %s --input-file=%t.serial --implicit-check-not='warning:' --implicit-check-not='error:' --implicit-check-not='note:'
// RUN: cir-opt %t.cir --cir-canonicalize --mlir-disable-threading=false --cir-lifetime-check="history=all" -o /dev/null 2> %t.parallel
// RUN: diff %t.serial %t.parallel
// RUN: cir-opt %t.cir --cir-canonicalize --emit-bytecode -o %t.bc
// RUN: cir-opt %t.bc --mlir-disable-threading=false --cir-lifetime-check="history=all" -o /dev/null 2> %t.bytecode
// RUN: diff %t.serial %t.bytecode

// Several analyses read the same callee and record metadata. Mutable type
// caches, histories, and owner generations must remain function-local.
struct [[gsl::Owner(int)]] Owner {
  Owner();
  void mutate();
};
struct [[gsl::Pointer(int)]] Pointer {
  Pointer(const Owner &);
  int read() const;
};
void consume(int &&);

int first_owner() {
  Owner owner;
  Pointer pointer(owner);
  owner.mutate(); // expected-note {{invalidated by non-const use of owner type}}
  return pointer.read(); // expected-warning {{use of invalid pointer 'pointer'}}
}
// CHECK: warning: use of invalid pointer 'pointer'
// CHECK: note: invalidated by non-const use of owner type

int first_move() {
  int value = 1;
  consume(static_cast<int &&>(value)); // expected-note {{moved here via std::move or rvalue reference}}
  return value; // expected-warning {{use of moved-from value 'value'}}
}
// CHECK: warning: use of moved-from value 'value'
// CHECK: note: moved here via std::move or rvalue reference

int quiet_owner() {
  Owner owner;
  Pointer pointer(owner);
  return pointer.read();
}

int second_owner() {
  Owner owner;
  Pointer pointer(owner);
  owner.mutate(); // expected-note {{invalidated by non-const use of owner type}}
  return pointer.read(); // expected-warning {{use of invalid pointer 'pointer'}}
}
// CHECK: warning: use of invalid pointer 'pointer'
// CHECK: note: invalidated by non-const use of owner type

int second_move() {
  int value = 2;
  consume(static_cast<int &&>(value)); // expected-note {{moved here via std::move or rvalue reference}}
  return value; // expected-warning {{use of moved-from value 'value'}}
}
// CHECK: warning: use of moved-from value 'value'
// CHECK: note: moved here via std::move or rvalue reference
