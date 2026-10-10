// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -std=c++17 -fclangir -fclangir-lifetime-check="history=all" -verify -emit-cir %s -o /dev/null
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -std=c++17 -fclangir -fclangir-lifetime-check -clangir-disable-passes -emit-cir %s -o %t.cir
// RUN: cir-opt %t.cir --cir-canonicalize --cir-lifetime-check="history=all" --verify-diagnostics -o /dev/null
// RUN: cir-opt %t.cir --emit-bytecode -o %t.bc
// RUN: cir-opt %t.bc --cir-lifetime-check="history=all" --verify-diagnostics -o /dev/null
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -std=c++14 -fclangir -fclangir-lifetime-check="history=all" -verify -emit-cir %s -o /dev/null
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -std=c++14 -fclangir -fclangir-lifetime-check -clangir-disable-passes -emit-cir %s -o %t.cxx14.cir
// RUN: cir-opt %t.cxx14.cir --cir-canonicalize --cir-lifetime-check="history=all" --verify-diagnostics -o /dev/null
// RUN: cir-opt %t.cxx14.cir --emit-bytecode -o %t.cxx14.bc
// RUN: cir-opt %t.cxx14.bc --cir-lifetime-check="history=all" --verify-diagnostics -o /dev/null

struct Record { unsigned first, second; };
Record get();
void take(Record, Record, Record, Record, Record);
void take(Record, Record);
struct Input { Record get() const; };

// An aggregate argument reload snapshots its completed construction. Keep
// several opaque record-valued calls to exercise the materialized arguments.
#if __cplusplus >= 201703L
void five_record_arguments(const Input &input) {
  take(input.get(), input.get(), input.get(), input.get(), input.get());
}
#endif

int observe(int);
void consume(int &&);
Record get(int);

// A completed move in an earlier statement makes the later argument read
// invalid, independently of the order chosen for the call's arguments.
void effectful_arguments() {
  int value = 0;
  consume(static_cast<int &&>(value)); // expected-note {{moved here}}
  take(get(),
       get(observe(value)) // expected-warning {{use of moved-from value}}
#if __cplusplus >= 201703L
       , get(), get(), get()
#endif
  );
}

void repeated(Record, int, Record);
void repeated_source_object() {
  Record value{1, 2};
  // These two copies read the same object, with argument materialization
  // following the order emitted in CIR.
  repeated(value, (value.first = 3, 0), value);
}

struct Nontrivial {
  Nontrivial();
  Nontrivial(const Nontrivial &);
  ~Nontrivial();
};
Nontrivial make();
void take(Nontrivial, Nontrivial);
void temporary_cleanup() { take(make(), make()); }
