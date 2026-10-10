// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -std=c++23 -fexceptions -fcxx-exceptions -fclangir -fclangir-lifetime-check="history=all" -verify -emit-cir %s -o /dev/null

void consume(int &&) noexcept;
void observe(int) noexcept;
void may_throw();

void move_before_throw() {
  int value = 1;
  try {
    consume(static_cast<int &&>(value)); // expected-note {{moved here via std::move or rvalue reference}}
    throw 1;
  } catch (int) {
    observe(value); // expected-warning {{use of moved-from value 'value'}}
  }
}

void reset_after_possible_throw() {
  int value = 1;
  try {
    consume(static_cast<int &&>(value)); // expected-note {{moved here via std::move or rvalue reference}}
    may_throw();
    value = 2;
  } catch (...) {
    observe(value); // expected-warning {{use of moved-from value 'value'}}
  }
}

void throwing_path_reset() {
  int value = 1;
  try {
    consume(static_cast<int &&>(value));
    throw 1;
  } catch (int) {
    value = 2;
  }
  observe(value);
}

void throw_before_move() {
  int value = 1;
  try {
    may_throw();
    consume(static_cast<int &&>(value));
  } catch (...) {
    observe(value);
  }
}

void wrong_handler_does_not_reset() {
  int value = 1;
  try {
    consume(static_cast<int &&>(value)); // expected-note {{moved here via std::move or rvalue reference}}
    throw 1;
  } catch (float) {
    value = 2;
  } catch (int) {
    observe(value); // expected-warning {{use of moved-from value 'value'}}
  }
}

void consume_and_maybe_throw(int &&);

void consuming_call_may_throw_after_consumption() {
  int value = 1;
  try {
    consume_and_maybe_throw(static_cast<int &&>(value)); // expected-note {{moved here via std::move or rvalue reference}}
    value = 2;
  } catch (...) {
    observe(value); // expected-warning {{use of moved-from value 'value'}}
  }
}

void nested_rethrow() {
  int value = 1;
  try {
    try {
      consume(static_cast<int &&>(value)); // expected-note {{moved here via std::move or rvalue reference}}
      throw 1;
    } catch (int) {
      throw;
    }
  } catch (int) {
    observe(value); // expected-warning {{use of moved-from value 'value'}}
  }
}

struct BaseException {};
struct DerivedException : BaseException {};

void derived_exception_reaches_base_handler() {
  int value = 1;
  try {
    consume(static_cast<int &&>(value)); // expected-note {{moved here via std::move or rvalue reference}}
    throw DerivedException{};
  } catch (const BaseException &) {
    observe(value); // expected-warning {{use of moved-from value 'value'}}
  }
}
