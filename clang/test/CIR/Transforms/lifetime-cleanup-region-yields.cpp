// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -std=c++14 -fclangir -fclangir-lifetime-check -verify -emit-cir %s -o /dev/null
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -std=c++17 -fclangir -fclangir-lifetime-check -verify -emit-cir %s -o /dev/null
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -std=c++17 -fclangir -fclangir-lifetime-check -clangir-disable-passes -emit-cir %s -o %t.cir
// RUN: cir-opt %t.cir --cir-canonicalize --cir-lifetime-check --verify-diagnostics -o /dev/null
// RUN: cir-opt %t.cir --emit-bytecode -o %t.bc
// RUN: cir-opt %t.bc --cir-lifetime-check --verify-diagnostics -o /dev/null
// expected-no-diagnostics

struct Record { unsigned value; };
Record get();
bool equal(const Record &, const Record &);
void failure(const char *, const char *, unsigned, const char *);

// Reduced from APINotes' assertion after setting an optional enum. Nested
// full-expression cleanup bodies each have a yield. Each body executes
// before its terminator and enclosing cleanup.
void nested_cleanup_terminators(Record value) {
  (equal(get(), value) && "bad")
      ? void(0)
      : failure("condition", "file", 1, "function");
}
