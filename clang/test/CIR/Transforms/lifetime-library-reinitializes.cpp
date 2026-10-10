// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -std=c++20 -fexceptions -fcxx-exceptions -fclangir -fclangir-lifetime-check="history=all" -verify -emit-cir %s -o %t.cir
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -std=c++20 -fexceptions -fcxx-exceptions -fclangir -fclangir-lifetime-check -clangir-disable-passes -emit-cir %s -o %t.raw.cir
// RUN: cir-opt %t.raw.cir --cir-canonicalize --cir-lifetime-check="history=all" -o /dev/null 2>&1 | FileCheck %s

namespace std {
template <class T> T &&move(T &value) { return static_cast<T &&>(value); }
}
struct Resource {
  Resource() noexcept;
  [[clang::reinitializes]] void restore() noexcept;
  [[clang::reinitializes]] void restore_may_throw();
  void use() const noexcept;
};
void consume(Resource &&) noexcept;
void unknown_mutable_call(Resource &) noexcept;

void restore_after_move() {
  Resource source;
  consume(std::move(source));
  source.restore();
  source.use();
}

void mutable_argument_does_not_prove_restore() {
  Resource source;
  consume(std::move(source)); // expected-note 2 {{moved here via std::move or rvalue reference}}
  unknown_mutable_call(source); // expected-warning {{use of moved-from value 'source'}}
  source.use(); // expected-warning {{use of moved-from value 'source'}}
}

void restore_only_on_normal_completion() {
  Resource source;
  consume(std::move(source)); // expected-note {{moved here via std::move or rvalue reference}}
  try {
    source.restore_may_throw();
    source.use();
  } catch (...) {
    source.use(); // expected-warning {{use of moved-from value 'source'}}
  }
}

// CHECK: warning: use of moved-from value 'source'
// CHECK: warning: use of moved-from value 'source'
