// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -std=c++23 -fclangir -fclangir-lifetime-check="history=all" -verify -emit-cir %s -o /dev/null

void consume(int &&);
void observe(int);

void conditional_move(bool condition) {
  int value = 1;
  condition ? consume(static_cast<int &&>(value)) : (void)0; // expected-note {{moved here via std::move or rvalue reference}}
  observe(value); // expected-warning {{use of moved-from value 'value'}}
}

void conditional_use(bool condition) {
  int value = 1;
  consume(static_cast<int &&>(value)); // expected-note {{moved here via std::move or rvalue reference}}
  condition ? observe(value) : (void)0; // expected-warning {{use of moved-from value 'value'}}
}

void conditional_reinitialization(bool condition) {
  int value = 1;
  consume(static_cast<int &&>(value));
  condition ? (value = 2) : (value = 3);
  observe(value);
}

void partial_reinitialization(bool condition) {
  int value = 1;
  consume(static_cast<int &&>(value)); // expected-note {{moved here via std::move or rvalue reference}}
  if (condition)
    value = 2;
  observe(value); // expected-warning {{use of moved-from value 'value'}}
}

void increment_only_move(bool condition) {
  int value = 1;
  for (; condition; consume(static_cast<int &&>(value))) { // expected-note {{moved here via std::move or rvalue reference}}
    observe(value); // expected-warning {{use of moved-from value 'value'}}
    value = 2;
  }
}

void continue_runs_increment(bool condition) {
  int value = 1;
  for (; condition; consume(static_cast<int &&>(value))) { // expected-note {{moved here via std::move or rvalue reference}}
    observe(value); // expected-warning {{use of moved-from value 'value'}}
    value = 2;
    continue;
  }
}

void break_skips_increment(bool condition) {
  int value = 1;
  for (; condition; consume(static_cast<int &&>(value)))
    break;
  observe(value);
}

void returning_path_does_not_join(bool condition) {
  int value = 1;
  if (condition) {
    consume(static_cast<int &&>(value));
    return;
  }
  observe(value);
}

void aliases_join(bool condition) {
  int value = 1, other = 2;
  int *p = condition ? &value : &other;
  consume(static_cast<int &&>(*p)); // expected-note {{moved here via std::move or rvalue reference}}
  *p = 3; // This write need not overwrite value on every path.
  observe(value); // expected-warning {{use of moved-from value 'value'}}
}

extern int *external_pointer;

int global_address_keeps_local_write_conditional(bool condition) {
  int value = 1;
  int *local = nullptr; // expected-note {{'nullptr' invalidated here}}
  int **destination = condition ? &local : &external_pointer;
  *destination = &value;
  return *local; // expected-warning {{use of invalid pointer 'local'}}
}

void global_pointer_load_keeps_reset_conditional(bool condition) {
  int value = 1;
  consume(static_cast<int &&>(value)); // expected-note {{moved here via std::move or rvalue reference}}
  int *local = &value;
  int **destination = condition ? &local : &external_pointer;
  **destination = 3;
  observe(value); // expected-warning {{use of moved-from value 'value'}}
}
