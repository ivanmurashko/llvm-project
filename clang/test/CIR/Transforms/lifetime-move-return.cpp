// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -std=c++20 -fclangir -fclangir-lifetime-check="history=all" -emit-cir %s -o /dev/null -verify

void consume(int &&);
void observe(int &);

void move_then_return(bool condition) {
  int value = 0;
  if (condition) {
    consume(static_cast<int &&>(value));
    return;
  }
  int copy = value;
}

void move_else_return(bool condition) {
  int value = 0;
  if (condition) {
    observe(value);
  } else {
    consume(static_cast<int &&>(value));
    return;
  }
  int copy = value;
}

void move_without_return(bool condition) {
  int value = 0;
  if (condition) {
    consume(static_cast<int &&>(value)); // expected-note {{moved here via std::move or rvalue reference}}
  }
  int copy = value; // expected-warning {{use of moved-from value 'value'}}
}
