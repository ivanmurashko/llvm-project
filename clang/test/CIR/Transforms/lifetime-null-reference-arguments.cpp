// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -std=c++17 -fclangir -fclangir-lifetime-check="history=all" -verify -emit-cir %s -o /dev/null
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -std=c++17 -fclangir -fclangir-lifetime-check -clangir-disable-passes -emit-cir %s -o %t.cir
// RUN: cir-opt %t.cir --cir-canonicalize --cir-lifetime-check="history=all" -o /dev/null 2>&1 | FileCheck %s --implicit-check-not="warning:" --implicit-check-not="error:"
// RUN: cir-opt %t.cir --emit-bytecode -o %t.bc
// RUN: cir-opt %t.bc --cir-lifetime-check="history=all" -o /dev/null 2>&1 | FileCheck %s --implicit-check-not="warning:" --implicit-check-not="error:"

struct [[gsl::Owner(int)]] Owner { Owner() noexcept; };
struct [[gsl::Pointer(int)]] View {
  int *data;
  View() noexcept;
  View(View &&) noexcept;
  View(Owner &) noexcept;
  int read() const noexcept;
};
bool output(View &) noexcept;
void inspect(const View &) noexcept;
struct Receiver { void output(int, View &) noexcept; };

void empty_method_argument(Receiver &receiver) {
  View view;
  receiver.output(0, view);
}
void replace_pointer(int *&) noexcept;
void inspect_pointer(int *const &) noexcept;
int raw_pointer_reference() {
  int *pointer = nullptr; // expected-note {{'nullptr' invalidated here}}
  replace_pointer(pointer);
  return *pointer; // expected-warning {{use of invalid pointer 'pointer'}}
}
int empty_reference() {
  View view; // expected-note {{'nullptr' invalidated here}}
  output(view);
  inspect(view);
  return view.read(); // expected-warning {{use of invalid pointer 'view'}}
}

// Use a local move constructor supported in C4; incoming record-value and
// pointer-payload constructor modeling are C5 features and are not prerequisites.
void moved_reference() {
  Owner owner;
  View view(owner);
  View destination(static_cast<View &&>(view)); // expected-note 3 {{moved here via std::move or rvalue reference}}
  output(view); // expected-warning {{passing invalid pointer 'view'}}
  inspect(view); // expected-warning {{passing invalid pointer 'view'}}
  view.read(); // expected-warning {{use of invalid pointer 'view'}}
}
int dangling_reference() {
  int *pointer;
  {
    int local = 1;
    pointer = &local;
  } // expected-note 3 {{pointee 'local' invalidated at end of scope}}
  replace_pointer(pointer); // expected-warning {{passing invalid pointer 'pointer'}}
  inspect_pointer(pointer); // expected-warning {{passing invalid pointer 'pointer'}}
  return *pointer; // expected-warning {{use of invalid pointer 'pointer'}}
}

// CHECK: warning: use of invalid pointer 'pointer'
// CHECK: note: 'nullptr' invalidated here
// CHECK: warning: use of invalid pointer 'view'
// CHECK: note: 'nullptr' invalidated here
// CHECK: warning: passing invalid pointer 'view'
// CHECK: note: moved here via std::move or rvalue reference
// CHECK: warning: passing invalid pointer 'view'
// CHECK: note: moved here via std::move or rvalue reference
// CHECK: warning: use of invalid pointer 'view'
// CHECK: note: moved here via std::move or rvalue reference
// CHECK: warning: passing invalid pointer 'pointer'
// CHECK: note: pointee 'local' invalidated at end of scope
// CHECK: warning: passing invalid pointer 'pointer'
// CHECK: note: pointee 'local' invalidated at end of scope
// CHECK: warning: use of invalid pointer 'pointer'
// CHECK: note: pointee 'local' invalidated at end of scope
