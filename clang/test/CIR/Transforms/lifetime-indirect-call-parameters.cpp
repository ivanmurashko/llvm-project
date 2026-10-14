// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -std=c++20 -fclangir -fclangir-lifetime-check="history=all" -verify -emit-cir %s -o /dev/null
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -std=c++20 -fclangir -fclangir-lifetime-check -clangir-disable-passes -emit-cir %s -o %t.cir
// RUN: cir-opt %t.cir --mlir-disable-threading=true --cir-lifetime-check="history=all" -o /dev/null 2> %t.serial
// RUN: FileCheck %s --check-prefix=CIR --input-file=%t.serial --implicit-check-not='warning:' --implicit-check-not='error:' --implicit-check-not='remark:' --implicit-check-not='note:'
// RUN: cir-opt %t.cir --mlir-disable-threading=false --cir-lifetime-check="history=all" -o /dev/null 2> %t.parallel
// RUN: diff %t.serial %t.parallel
// RUN: cir-opt %t.cir --cir-canonicalize --mlir-print-debuginfo -o %t.canonical.cir
// RUN: cir-opt %t.canonical.cir --mlir-disable-threading=true --cir-lifetime-check="history=all" -o /dev/null 2> %t.canonical.serial
// RUN: diff %t.serial %t.canonical.serial
// RUN: cir-opt %t.canonical.cir --mlir-disable-threading=false --cir-lifetime-check="history=all" -o /dev/null 2> %t.canonical.parallel
// RUN: diff %t.serial %t.canonical.parallel
// RUN: cir-opt %t.canonical.cir --emit-bytecode -o %t.bc
// RUN: cir-opt %t.bc --mlir-disable-threading=true --cir-lifetime-check="history=all" -o /dev/null 2> %t.bytecode.serial
// RUN: diff %t.serial %t.bytecode.serial
// RUN: cir-opt %t.bc --mlir-disable-threading=false --cir-lifetime-check="history=all" -o /dev/null 2> %t.bytecode.parallel
// RUN: diff %t.serial %t.bytecode.parallel

namespace std {
template <class T> T &&move(T &value) noexcept {
  return static_cast<T &&>(value);
}
} // namespace std

struct Item {
  Item();
  Item(Item &&) noexcept;
  int member;
};
void observe(const Item &) noexcept;
void observe_int(int) noexcept;

// This target really invokes a move constructor. The caller still uses only
// its serialized parameter binding, without inspecting the target body.
void consuming_target(Item &&input) {
  Item owned(static_cast<Item &&>(input));
  observe(owned);
}

void through_function_pointer() {
  Item value;
  auto callee = &consuming_target;
  callee(std::move(value)); // expected-note {{moved here}}
  observe(value); // expected-warning {{use of moved-from value 'value'}}
}

void through_function_reference(void (&callee)(Item &&)) {
  Item value;
  callee(static_cast<Item &&>(value)); // expected-note {{moved here}}
  observe(value); // expected-warning {{use of moved-from value 'value'}}
}

void mixed_positions(void (*callee)(int, const Item &, Item &&)) {
  Item live;
  Item moved;
  callee(3, live, static_cast<Item &&>(moved)); // expected-note {{moved here}}
  observe(live);
  observe(moved); // expected-warning {{use of moved-from value 'moved'}}
}

void local_alias(void (*callee)(Item &&)) {
  Item value;
  Item &alias = value;
  callee(static_cast<Item &&>(alias)); // expected-note {{moved here}}
  observe(value); // expected-warning {{use of moved-from value 'value'}}
}

void maybe_consumed(bool condition, void (*callee)(Item &&)) {
  Item value;
  if (condition)
    callee(static_cast<Item &&>(value)); // expected-note {{moved here}}
  observe(value); // expected-warning {{use of moved-from value 'value'}}
}

// A possible target remains possible after joining a local reference.
void may_target(bool condition, void (*callee)(Item &&)) {
  Item first;
  Item second;
  Item &alias = condition ? first : second;
  callee(static_cast<Item &&>(alias)); // expected-note 2 {{moved here}}
  observe(first); // expected-warning {{use of moved-from value 'first'}}
  observe(second); // expected-warning {{use of moved-from value 'second'}}
}

void observer_does_not_consume(void (*callee)(const Item &)) {
  Item value;
  callee(value);
  observe(value);
}

void moved_value_reaches_indirect_observer(void (*callee)(const Item &)) {
  Item value;
  consuming_target(static_cast<Item &&>(value)); // expected-note {{moved here}}
  callee(value); // expected-warning {{use of moved-from value 'value'}}
}

void reset_after_indirect_move(void (*callee)(int &&)) {
  int value = 1;
  callee(static_cast<int &&>(value));
  value = 2;
  observe_int(value);
}

// Merely producing or binding an xvalue is not a consuming operation.
void casts_and_binding_are_not_consumption() {
  Item value;
  (void)std::move(value);
  (void)static_cast<Item &&>(value);
  Item &&alias = static_cast<Item &&>(value);
  observe(alias);
  observe(value);
}

// Current CIR policy treats every ordinary T&& call as potentially consuming.
// This deliberately no-op target therefore has the same known false positive
// through a direct call and a function pointer. It is not interprocedural proof.
void no_op(Item &&) noexcept {}
void conservative_direct_no_op() {
  Item value;
  no_op(static_cast<Item &&>(value)); // expected-note {{moved here}}
  observe(value); // expected-warning {{use of moved-from value 'value'}}
}
void conservative_indirect_no_op() {
  Item value;
  auto callee = &no_op;
  callee(static_cast<Item &&>(value)); // expected-note {{moved here}}
  observe(value); // expected-warning {{use of moved-from value 'value'}}
}

// CIR: {{^.*}}lifetime-indirect-call-parameters.cpp:43:{{[0-9]+}}: warning: use of moved-from value 'value'
// CIR: {{^.*}}lifetime-indirect-call-parameters.cpp:42:{{[0-9]+}}: note: moved here
// CIR: {{^.*}}lifetime-indirect-call-parameters.cpp:49:{{[0-9]+}}: warning: use of moved-from value 'value'
// CIR: {{^.*}}lifetime-indirect-call-parameters.cpp:48:{{[0-9]+}}: note: moved here
// CIR: {{^.*}}lifetime-indirect-call-parameters.cpp:57:{{[0-9]+}}: warning: use of moved-from value 'moved'
// CIR: {{^.*}}lifetime-indirect-call-parameters.cpp:55:{{[0-9]+}}: note: moved here
// CIR: {{^.*}}lifetime-indirect-call-parameters.cpp:64:{{[0-9]+}}: warning: use of moved-from value 'value'
// CIR: {{^.*}}lifetime-indirect-call-parameters.cpp:63:{{[0-9]+}}: note: moved here
// CIR: {{^.*}}lifetime-indirect-call-parameters.cpp:71:{{[0-9]+}}: warning: use of moved-from value 'value'
// CIR: {{^.*}}lifetime-indirect-call-parameters.cpp:70:{{[0-9]+}}: note: moved here
// CIR: {{^.*}}lifetime-indirect-call-parameters.cpp:80:{{[0-9]+}}: warning: use of moved-from value 'first'
// CIR: {{^.*}}lifetime-indirect-call-parameters.cpp:79:{{[0-9]+}}: note: moved here
// CIR: {{^.*}}lifetime-indirect-call-parameters.cpp:81:{{[0-9]+}}: warning: use of moved-from value 'second'
// CIR: {{^.*}}lifetime-indirect-call-parameters.cpp:79:{{[0-9]+}}: note: moved here
// CIR: {{^.*}}lifetime-indirect-call-parameters.cpp:93:{{[0-9]+}}: warning: use of moved-from value 'value'
// CIR: {{^.*}}lifetime-indirect-call-parameters.cpp:92:{{[0-9]+}}: note: moved here
// CIR: {{^.*}}lifetime-indirect-call-parameters.cpp:120:{{[0-9]+}}: warning: use of moved-from value 'value'
// CIR: {{^.*}}lifetime-indirect-call-parameters.cpp:119:{{[0-9]+}}: note: moved here
// CIR: {{^.*}}lifetime-indirect-call-parameters.cpp:126:{{[0-9]+}}: warning: use of moved-from value 'value'
// CIR: {{^.*}}lifetime-indirect-call-parameters.cpp:125:{{[0-9]+}}: note: moved here
