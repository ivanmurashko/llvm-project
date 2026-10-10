// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -std=c++23 -mconstructor-aliases -fclangir -fclangir-lifetime-check="history=all" -verify -emit-cir %s -o /dev/null
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -std=c++23 -mconstructor-aliases -fclangir -fclangir-lifetime-check -clangir-disable-passes -emit-cir %s -o %t.cir
// RUN: cir-opt %t.cir --cir-canonicalize --cir-lifetime-check="history=all" -o /dev/null 2>&1 | FileCheck %s --check-prefix=REPARSED
// REPARSED: warning: use of moved-from value 'source'

namespace std {
template <class T> T &&move(T &value) { return static_cast<T &&>(value); }
template <class T> T &&forward(T &value) { return static_cast<T &&>(value); }
}
void consume(int &&);
void consume(bool &&);
void consume(double &&);
void observe(int);
void observe(bool);
void observe(double);

void scalar_reinitialization() {
  int value = 1;
  consume(std::move(value));
  value = 2;
  observe(value);
  consume(std::move(value)); // expected-note {{moved here via std::move or rvalue reference}}
  observe(value); // expected-warning {{use of moved-from value 'value'}}
}

void scalar_kinds() {
  bool flag = true;
  consume(std::move(flag));
  flag = false;
  observe(flag);
  double value = 1.0;
  consume(std::move(value));
  value = 2.0;
  observe(value);
}

void reference_alias() {
  int source = 1;
  int &alias = source;
  consume(std::move(alias)); // expected-note {{moved here via std::move or rvalue reference}}
  observe(source); // expected-warning {{use of moved-from value 'source'}}
  alias = 2;
  observe(source);
}

void pointer_alias() {
  int source = 1;
  int *alias = &source;
  consume(std::move(*alias)); // expected-note {{moved here via std::move or rvalue reference}}
  observe(source); // expected-warning {{use of moved-from value 'source'}}
  *alias = 2;
  observe(source);
}

// q retains the value read from p, rather than following p's subsequent store.
void alias_snapshot() {
  int source = 1, other = 2;
  int *p = &source;
  int *q = p;
  p = &other;
  consume(std::move(*q)); // expected-note {{moved here via std::move or rvalue reference}}
  observe(source); // expected-warning {{use of moved-from value 'source'}}
  observe(other);
}

void forwarding_parameter(int &&source) {
  consume(std::forward<int>(source)); // expected-note {{moved here via std::move or rvalue reference}}
  observe(source); // expected-warning {{use of moved-from value 'source'}}
  source = 3;
  observe(source);
}

void scalar_move_capture() {
  int source = 1;
  auto closure = [captured = std::move(source)] {}; // expected-note {{moved here via std::move or rvalue reference}}
  observe(source); // expected-warning {{use of moved-from value 'source'}}
}

void scalar_capture_controls() {
  int source = 1;
  auto copied = [captured = source] {};
  auto referenced = [&source] {};
  observe(source);
}

struct Value {
  int member;
  Value();
  Value(Value &&);
  Value &operator=(Value &&);
  void use() const;
};
void consume(Value &&);

void whole_object_use() {
  Value source;
  consume(std::move(source)); // expected-note {{moved here via std::move or rvalue reference}}
  source.use(); // expected-warning {{use of moved-from value 'source'}}
  source = Value();
  source.use();
}

void inspect(const Value &);

void whole_object_argument_use() {
  Value source;
  consume(std::move(source)); // expected-note {{moved here via std::move or rvalue reference}}
  inspect(source); // expected-warning {{use of moved-from value 'source'}}
}

int whole_object_member_read() {
  Value source;
  consume(std::move(source)); // expected-note {{moved here via std::move or rvalue reference}}
  return source.member; // expected-warning {{use of moved-from value 'source'}}
}
