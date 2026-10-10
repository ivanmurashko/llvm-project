// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -std=c++17 -fclangir -fclangir-lifetime-check="history=all" -verify -emit-cir %s -o /dev/null
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -std=c++17 -fclangir -fclangir-lifetime-check -clangir-disable-passes -emit-cir %s -o %t.cir
// RUN: cir-opt %t.cir --cir-canonicalize --cir-lifetime-check="history=all" --verify-diagnostics -o /dev/null
// RUN: cir-opt %t.cir --cir-canonicalize --emit-bytecode -o %t.bc
// RUN: cir-opt %t.bc --cir-lifetime-check="history=all" --verify-diagnostics -o /dev/null

// TokenKinds::isLiteral exposed lost boolean results between sequential
// short-circuit regions. These expressions have only linear possible paths.
bool long_or(int kind) {
  return kind == 0 || kind == 1 || kind == 2 || kind == 3 || kind == 4 ||
         kind == 5 || kind == 6 || kind == 7 || kind == 8;
}
bool long_and(int kind) {
  return kind != 0 && kind != 1 && kind != 2 && kind != 3 && kind != 4 &&
         kind != 5 && kind != 6 && kind != 7 && kind != 8;
}

void consume(int &&) noexcept;
void observe(int) noexcept;
bool unknown_bool() noexcept;
void write_bool(bool &) noexcept;
void escape(bool *) noexcept;
void mutate_escaped() noexcept;

void unreachable_or_arm(int kind) {
  int value = 1;
  (void)((kind == 0 || (consume(static_cast<int &&>(value)), true)) ||
         (observe(value), true));
}
void reachable_or_arm(int kind) {
  int value = 1;
  (void)((kind == 0 || (consume(static_cast<int &&>(value)), false)) || // expected-note {{moved here via std::move or rvalue reference}}
         (observe(value), true)); // expected-warning {{use of moved-from value 'value'}}
}
void unreachable_and_arm(int kind) {
  int value = 1;
  (void)((kind == 0 && (consume(static_cast<int &&>(value)), false)) &&
         (observe(value), true));
}
void reachable_and_arm(int kind) {
  int value = 1;
  (void)((kind == 0 && (consume(static_cast<int &&>(value)), true)) && // expected-note {{moved here via std::move or rvalue reference}}
         (observe(value), true)); // expected-warning {{use of moved-from value 'value'}}
}

void equal_region_results(bool choice) {
  int value = 1;
  consume(static_cast<int &&>(value));
  bool flag = choice ? true : true;
  if (!flag)
    observe(value);
}
void different_region_results(bool choice) {
  int value = 1;
  consume(static_cast<int &&>(value)); // expected-note {{moved here via std::move or rvalue reference}}
  bool flag = choice ? true : false;
  if (!flag)
    observe(value); // expected-warning {{use of moved-from value 'value'}}
}
void unknown_region_result(bool choice) {
  int value = 1;
  consume(static_cast<int &&>(value)); // expected-note {{moved here via std::move or rvalue reference}}
  bool flag = choice ? true : unknown_bool();
  if (!flag)
    observe(value); // expected-warning {{use of moved-from value 'value'}}
}
void replaced_boolean() {
  int value = 1;
  consume(static_cast<int &&>(value));
  bool flag = false;
  flag = true;
  if (!flag)
    observe(value);
}
void conditional_replacement(bool choice) {
  int value = 1;
  consume(static_cast<int &&>(value)); // expected-note {{moved here via std::move or rvalue reference}}
  bool flag = false;
  if (choice)
    flag = true;
  if (!flag)
    observe(value); // expected-warning {{use of moved-from value 'value'}}
}
extern bool outside;
void uncertain_alias_write(bool choice) {
  int value = 1;
  consume(static_cast<int &&>(value)); // expected-note {{moved here via std::move or rvalue reference}}
  bool flag = false;
  bool *address = choice ? &flag : &outside;
  *address = true;
  if (!flag)
    observe(value); // expected-warning {{use of moved-from value 'value'}}
}
void opaque_reference_write() {
  int value = 1;
  consume(static_cast<int &&>(value)); // expected-note {{moved here via std::move or rvalue reference}}
  bool flag = true;
  write_bool(flag);
  if (!flag)
    observe(value); // expected-warning {{use of moved-from value 'value'}}
}
void previously_escaped_write() {
  int value = 1;
  consume(static_cast<int &&>(value)); // expected-note {{moved here via std::move or rvalue reference}}
  bool flag = false;
  escape(&flag);
  flag = true;
  mutate_escaped();
  if (!flag)
    observe(value); // expected-warning {{use of moved-from value 'value'}}
}
void volatile_read() {
  int value = 1;
  consume(static_cast<int &&>(value)); // expected-note {{moved here via std::move or rvalue reference}}
  volatile bool flag = true;
  if (!flag)
    observe(value); // expected-warning {{use of moved-from value 'value'}}
}
void loop_boolean_update(bool again) {
  int value = 1;
  consume(static_cast<int &&>(value)); // expected-note {{moved here via std::move or rvalue reference}}
  bool flag = false;
  do {
    if (flag)
      observe(value); // expected-warning {{use of moved-from value 'value'}}
    flag = true;
  } while (again);
}
void loop_boolean_unchanged(bool again) {
  int value = 1;
  consume(static_cast<int &&>(value));
  bool flag = false;
  while (again) {
    if (flag)
      observe(value);
  }
}

// The call forgets prior constants, but this later store establishes a new one
// before an unknown-address write. The unknown target may still be &flag.
bool *escape_and_return_alias(bool *) noexcept;
void unknown_address_write() {
  int value = 1;
  consume(static_cast<int &&>(value)); // expected-note {{moved here via std::move or rvalue reference}}
  bool flag = false;
  bool *address = escape_and_return_alias(&flag);
  flag = false;
  *address = true;
  if (flag)
    observe(value); // expected-warning {{use of moved-from value 'value'}}
}
void unmodeled_bulk_write() {
  int value = 1;
  consume(static_cast<int &&>(value)); // expected-note {{moved here via std::move or rvalue reference}}
  bool flag = false;
  bool replacement = true;
  __builtin_memcpy(&flag, &replacement, sizeof(flag));
  if (flag)
    observe(value); // expected-warning {{use of moved-from value 'value'}}
}
