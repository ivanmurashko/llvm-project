// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -std=c++17 -fclangir -fclangir-lifetime-check -verify -emit-cir %s -o /dev/null
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -std=c++17 -fclangir -fclangir-lifetime-check -clangir-disable-passes -emit-cir %s -o %t.cir
// RUN: cir-opt %t.cir --cir-canonicalize --cir-lifetime-check --verify-diagnostics -o /dev/null
// RUN: cir-opt %t.cir --emit-bytecode -o %t.bc
// RUN: cir-opt %t.bc --cir-lifetime-check --verify-diagnostics -o /dev/null
// expected-no-diagnostics

struct Record { unsigned first, second; };
struct Input { Record get() const; };
struct Result { Result(Record, Record, Record, Record, Record, bool, bool); };

// Record-valued calls and literal arguments are evaluated in the order
// recorded by CIR, including their argument materialization.
void independent_literal_arguments(const Input &input) {
  Result result(input.get(), input.get(), input.get(), input.get(), input.get(),
                false, false);
}

void take(const Result &, int);

// A nested constructor retains its argument materialization and temporary
// cleanup while supplying the outer call.
void nested_literal_arguments(const Input &input, int selector) {
  take(Result(input.get(), input.get(), input.get(), input.get(), input.get(),
              false, false),
       selector);
}
