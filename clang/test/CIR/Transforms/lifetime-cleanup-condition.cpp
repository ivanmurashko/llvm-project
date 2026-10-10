// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -std=c++14 -fclangir -fclangir-lifetime-check="history=all" -verify -emit-cir %s -o /dev/null
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -std=c++17 -fclangir -fclangir-lifetime-check="history=all" -verify -emit-cir %s -o /dev/null
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -std=c++17 -fclangir -fclangir-lifetime-check -clangir-disable-passes -emit-cir %s -o %t.cir
// RUN: cir-opt %t.cir --cir-canonicalize --cir-lifetime-check="history=all" --verify-roundtrip -o /dev/null
// RUN: cir-opt %t.cir --emit-bytecode -o %t.bc
// RUN: cir-opt %t.bc --cir-canonicalize --cir-lifetime-check="history=all" --verify-roundtrip -o /dev/null
// expected-no-diagnostics

struct Trivial {
  int value;
};
Trivial make();
bool check(const Trivial &);
void failure(int, int);

// The condition temporary creates a full-expression cleanup inside a lexical
// scope. The body must execute before cleanup and before leaving that scope.
void temporary_condition() {
  check(make()) ? void(0) : failure(1, 2);
}

struct Nontrivial {
  Nontrivial();
  ~Nontrivial();
};
bool check(const Nontrivial &);

void destructor_condition() {
  check(Nontrivial()) ? void(0) : failure(1, 2);
}

void consume(int &&);
void observe(int);

// Both continuations reinitialize the value. The cleanup scaffolding must not
// change branch selection or expose an intermediate state after the join.
void reinitializing_arms() {
  int value = 0;
  consume(static_cast<int &&>(value));
  check(make()) ? (value = 1, 0) : (value = 2, 0);
  observe(value);
}
