// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -std=c++14 -fclangir -fclangir-lifetime-check -verify -emit-cir %s -o /dev/null
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -std=c++17 -fclangir -fclangir-lifetime-check -verify -emit-cir %s -o /dev/null
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -std=c++17 -fclangir -fclangir-lifetime-check -clangir-disable-passes -emit-cir %s -o %t.cir
// RUN: cir-opt %t.cir --cir-canonicalize --cir-lifetime-check --verify-roundtrip -o /dev/null
// RUN: cir-opt %t.cir --emit-bytecode -o %t.bc
// RUN: cir-opt %t.bc --cir-canonicalize --cir-lifetime-check --verify-roundtrip -o /dev/null
// expected-no-diagnostics

struct Item {
  Item();
  ~Item();
};
struct Aggregate {
  bool flag;
  Item item;
  int count;
};
void sink(Aggregate);
void use(bool, bool);

// Partial aggregate construction registers cleanup for completed subobjects.
// Later calls and constants remain ordinary operations after that cleanup.
void first() {
  sink({true, {}, 0});
  use(false, true);
}

void second() {
  sink({true, {}, 0});
  use(false, true);
  use(false, true);
}

// The same cleanup scaffolding appears while constructing lambda captures.
void lambda() {
  auto closure = [item = Item{}] {};
  use(false, true);
  use(false, true);
}

template <class F> void invoke(F &&function) { function(); }

// Separately emitted lambda/template bodies retain their own construction
// and cleanup scopes.
void nested_function_emission() {
  invoke([] {
    sink({true, {}, 0});
    use(false, true);
    use(false, true);
  });
  use(false, true);
}
