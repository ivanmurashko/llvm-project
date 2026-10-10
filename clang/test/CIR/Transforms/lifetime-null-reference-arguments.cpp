// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -std=c++17 -fclangir -fclangir-lifetime-check="history=all" -verify -emit-cir %s -o /dev/null
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -std=c++17 -fclangir -fclangir-lifetime-check -clangir-disable-passes -emit-cir %s -o %t.cir
// RUN: cir-opt %t.cir --cir-canonicalize --cir-lifetime-check="history=all" -o /dev/null 2>&1 | FileCheck %s --implicit-check-not="warning:" --implicit-check-not="error:"
// RUN: cir-opt %t.cir --emit-bytecode -o %t.bc
// RUN: cir-opt %t.bc --cir-lifetime-check="history=all" -o /dev/null 2>&1 | FileCheck %s --implicit-check-not="warning:" --implicit-check-not="error:"

struct [[gsl::Pointer(int)]] View {
  int *data;
  View() noexcept;
  View(int *) noexcept;
  int read() const noexcept;
};
bool output(View &) noexcept;
void inspect(const View &) noexcept;
void consume(View &&) noexcept;

struct Receiver {
  void output(int, View &) noexcept;
};
void empty_method_argument(Receiver &receiver) {
  View view;
  receiver.output(0, view);
}

void replace_pointer(int *&) noexcept;
int raw_pointer_reference() {
  int *pointer = nullptr; // expected-note {{'nullptr' invalidated here}}
  replace_pointer(pointer);
  return *pointer; // expected-warning {{use of invalid pointer 'pointer'}}
}

// Binding either reference does not dereference the contents of an empty
// pointer-like object. Neither call proves that the contents were replaced.
int empty_reference() {
  View view; // expected-note {{'nullptr' invalidated here}}
  output(view);
  inspect(view);
  return view.read(); // expected-warning {{use of invalid pointer 'view'}}
}

// A possibly modifying reference call does not repair a moved-from object.
void moved_reference(View view) {
  consume(static_cast<View &&>(view)); // expected-note 3 {{moved here via std::move or rvalue reference}}
  output(view); // expected-warning {{passing invalid pointer 'view'}}
  inspect(view); // expected-warning {{passing invalid pointer 'view'}}
  view.read(); // expected-warning {{use of invalid pointer 'view'}}
}

// Null contents and dangling contents are distinct. Keep the latter's cause.
void dangling_reference() {
  View view;
  {
    int local = 1;
    view = View(&local);
  } // expected-note 3 {{pointee 'local' invalidated at end of scope}}
  output(view); // expected-warning {{passing invalid pointer 'view'}}
  inspect(view); // expected-warning {{passing invalid pointer 'view'}}
  view.read(); // expected-warning {{use of invalid pointer 'view'}}
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
// CHECK: warning: passing invalid pointer 'view'
// CHECK: note: pointee 'local' invalidated at end of scope
// CHECK: warning: passing invalid pointer 'view'
// CHECK: note: pointee 'local' invalidated at end of scope
// CHECK: warning: use of invalid pointer 'view'
// CHECK: note: pointee 'local' invalidated at end of scope
// CHECK-NOT: warning:
// CHECK-NOT: error:
