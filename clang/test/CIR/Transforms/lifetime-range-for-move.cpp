// RUN: %clang_cc1 -std=c++17 -triple x86_64-unknown-linux-gnu -fcxx-exceptions -fexceptions -fclangir -fclangir-lifetime-check="history=all" -verify=expected,clang -emit-cir %s -o /dev/null
// RUN: %clang_cc1 -std=c++17 -triple x86_64-unknown-linux-gnu -fcxx-exceptions -fexceptions -O3 -fclangir -fclangir-lifetime-check="history=all" -verify=expected,clang -emit-cir %s -o /dev/null
// RUN: %clang_cc1 -std=c++17 -triple x86_64-unknown-linux-gnu -fcxx-exceptions -fexceptions -fclangir -fclangir-lifetime-check -clangir-disable-passes -emit-cir %s -o %t.cir
// RUN: cir-opt %t.cir --cir-canonicalize --cir-lifetime-check="history=all" --verify-diagnostics -o /dev/null
// RUN: cir-opt %t.cir --cir-canonicalize --cir-lifetime-check="history=all" --verify-diagnostics --emit-bytecode -o %t.bc
// RUN: cir-opt %t.bc --cir-lifetime-check="history=all" --verify-diagnostics -o /dev/null
// RUN: %clang_cc1 -std=c++17 -triple x86_64-unknown-linux-gnu -fcxx-exceptions -fexceptions -O3 -fclangir -fclangir-lifetime-check -clangir-disable-passes -emit-cir %s -o %t.o3.cir
// RUN: cir-opt %t.o3.cir --cir-canonicalize --cir-lifetime-check="history=all" --verify-diagnostics -o /dev/null
// RUN: cir-opt %t.o3.cir --cir-canonicalize --cir-lifetime-check="history=all" --verify-diagnostics --emit-bytecode -o %t.o3.bc
// RUN: cir-opt %t.o3.bc --cir-lifetime-check="history=all" --verify-diagnostics -o /dev/null

struct Value {
  int field;
  Value();
  Value(Value &&);
  ~Value();
  void use();
};
Value make_value();
void consume(Value &&) noexcept;
bool choose();

void fresh_factory(int (&input)[4]) {
  for (int element : input) {
    Value current = make_value();
    current.use();
    consume(static_cast<Value &&>(current));
  }
}

struct Aggregate { int field; };
void consume(Aggregate &&) noexcept;

void fresh_aggregate(int (&input)[4]) {
  for (int element : input) {
    Aggregate current{element};
    consume(static_cast<Aggregate &&>(current));
  }
}

void nested_ranges(int (&input)[4]) {
  for (int outer : input) {
    Value current = make_value();
    for (int inner : input)
      current.use();
    consume(static_cast<Value &&>(current));
  }
}

void fresh_after_continue(int (&input)[4]) {
  for (int element : input) {
    Value current = make_value();
    current.use();
    consume(static_cast<Value &&>(current));
    continue;
  }
}

void different_transfer_branches(int (&input)[4]) {
  for (int element : input) {
    Value current = make_value();
    if (choose()) {
      consume(static_cast<Value &&>(current));
      continue;
    }
    current.use();
    consume(static_cast<Value &&>(current));
  }
}

// A new iteration must not repair an object declared outside the loop.
void genuinely_reused(int (&input)[4]) {
  Value current = make_value();
  for (int element : input) {
    current.use(); // expected-warning {{use of moved-from value 'current'}}
    consume(static_cast<Value &&>(current)); // expected-warning {{use of moved-from value 'current'}}
    // expected-note@-1 {{moved here via std::move or rvalue reference}}
    // clang-note@-2 {{moved here via std::move or rvalue reference}}
  }
}

void same_iteration_use(int (&input)[4]) {
  for (int element : input) {
    Value current = make_value();
    consume(static_cast<Value &&>(current)); // expected-note {{moved here via std::move or rvalue reference}}
    current.use(); // expected-warning {{use of moved-from value 'current'}}
  }
}

void observe(int);

// Unwinding ends the current iteration's storage but does not restore an
// outer object's state. The only throwing operation inside the range is the
// explicit throw, after the source was consumed and the borrow was created.
void throwing_iteration(int (&input)[4]) {
  Value outer = make_value();
  int initial = 0;
  int *escaped = &initial;
  try {
    for (int element : input) {
      int local = element;
      escaped = &local;
      consume(static_cast<Value &&>(outer)); // expected-note {{moved here via std::move or rvalue reference}}
      throw 1;
    } // expected-note {{pointee 'local' invalidated at end of scope}}
  } catch (...) {
    observe(*escaped); // expected-warning {{use of invalid pointer 'escaped'}}
    outer.use(); // expected-warning {{use of moved-from value 'outer'}}
  }
}
