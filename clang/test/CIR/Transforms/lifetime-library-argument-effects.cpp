// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -std=c++20 -fexceptions -fcxx-exceptions -fclangir -fclangir-lifetime-check="history=all" -verify -emit-cir %s -o /dev/null
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -std=c++20 -fexceptions -fcxx-exceptions -fclangir -fclangir-lifetime-check -clangir-disable-passes -emit-cir %s -o %t.cir
// RUN: cir-opt %t.cir --cir-canonicalize --cir-lifetime-check="history=all" -o /dev/null 2>&1 | FileCheck %s
// RUN: cir-opt %t.cir --emit-bytecode -o %t.bc
// RUN: cir-opt %t.bc --cir-canonicalize --cir-lifetime-check="history=all" -o /dev/null 2>&1 | FileCheck %s

// A recognized library contract models some of its arguments. The remaining
// reference arguments keep the effects of any other call: reading a moved-from
// object is a use, and an rvalue reference consumes its object on both the
// normal and the exceptional continuation.
namespace std {
template <class T> T &&move(T &value) { return static_cast<T &&>(value); }
template <class A, class B> struct pair { A first; B second; };
template <class T> class [[gsl::Owner(T)]] optional {
public:
  optional() noexcept;
  optional(const T &) noexcept;
  optional(T &&);
  optional &operator=(T &&) noexcept;
  T &operator*() noexcept;
};
template <class K, class V> class map {
public:
  struct iterator { int position; };
  map() noexcept;
  pair<iterator, bool> try_emplace(const K &, V &&);
};
} // namespace std

struct Resource {
  Resource() noexcept;
  Resource(const Resource &) noexcept;
  Resource(Resource &&) noexcept;
  [[clang::reinitializes]] void restore_from(Resource &&) noexcept;
  [[clang::reinitializes]] void restore_from_may_throw(Resource &&);
  [[clang::reinitializes]] void restore_copy(const Resource &) noexcept;
  void use() const noexcept;
};

void consume(Resource &&) noexcept;

void value_constructor_consumes_argument() {
  Resource source;
  std::optional<Resource> holder(std::move(source)); // expected-note {{moved here via std::move or rvalue reference}}
  source.use(); // expected-warning {{use of moved-from value 'source'}}
}

void value_assignment_consumes_argument() {
  Resource source;
  std::optional<Resource> holder;
  holder = std::move(source); // expected-note {{moved here via std::move or rvalue reference}}
  source.use(); // expected-warning {{use of moved-from value 'source'}}
}

void value_constructor_reads_argument() {
  Resource source;
  consume(std::move(source)); // expected-note {{moved here via std::move or rvalue reference}}
  std::optional<Resource> holder(source); // expected-warning {{use of moved-from value 'source'}}
}

void consumption_reaches_exceptional_edge() {
  Resource source;
  try {
    std::optional<Resource> holder(std::move(source)); // expected-note {{moved here via std::move or rvalue reference}}
  } catch (...) {
    source.use(); // expected-warning {{use of moved-from value 'source'}}
  }
}

void reinitialization_consumes_argument() {
  Resource target, source;
  consume(std::move(target));
  target.restore_from(std::move(source)); // expected-note {{moved here via std::move or rvalue reference}}
  target.use();
  source.use(); // expected-warning {{use of moved-from value 'source'}}
}

// The receiver is restored only on normal completion; the argument may
// already have been consumed when the call throws.
void reinitialization_is_normal_only() {
  Resource target, source;
  consume(std::move(target)); // expected-note {{moved here via std::move or rvalue reference}}
  try {
    target.restore_from_may_throw(std::move(source)); // expected-note {{moved here via std::move or rvalue reference}}
    target.use();
  } catch (...) {
    target.use(); // expected-warning {{use of moved-from value 'target'}}
    source.use(); // expected-warning {{use of moved-from value 'source'}}
  }
}

// The argument is consumed before the receiver is reinitialized, even when
// both are the same object.
void reinitialization_from_itself_restores() {
  Resource value;
  value.restore_from(std::move(value));
  value.use();
}

void reinitialization_from_itself_is_normal_only() {
  Resource value;
  try {
    value.restore_from_may_throw(std::move(value)); // expected-note {{moved here via std::move or rvalue reference}}
    value.use();
  } catch (...) {
    value.use(); // expected-warning {{use of moved-from value 'value'}}
  }
}

// Arguments are checked in the state at call entry, before the receiver is
// reinitialized.
void reinitialization_from_moved_source() {
  Resource target, source;
  consume(std::move(source)); // expected-note {{moved here via std::move or rvalue reference}}
  target.restore_from(std::move(source)); // expected-warning {{use of moved-from value 'source'}}
}

void reinitialization_from_moved_receiver() {
  Resource value;
  consume(std::move(value)); // expected-note {{moved here via std::move or rvalue reference}}
  value.restore_from(std::move(value)); // expected-warning {{use of moved-from value 'value'}}
  value.use();
  Resource other;
  consume(std::move(other));
  other.restore_copy(value);
  other.use();
  consume(std::move(value)); // expected-note {{moved here via std::move or rvalue reference}}
  value.restore_copy(value); // expected-warning {{use of moved-from value 'value'}}
  value.use();
}

// try_emplace consumes its mapped argument only on successful insertion.
void try_emplace_stays_conditional() {
  std::map<int, Resource> values;
  Resource source;
  auto result = values.try_emplace(1, std::move(source));
  if (!result.second)
    source.use();
}

// CHECK: warning: use of moved-from value 'source'
// CHECK: warning: use of moved-from value 'source'
// CHECK: warning: use of moved-from value 'source'
// CHECK: warning: use of moved-from value 'source'
// CHECK: warning: use of moved-from value 'source'
// CHECK: warning: use of moved-from value 'target'
// CHECK: warning: use of moved-from value 'source'
// CHECK: warning: use of moved-from value 'value'
// CHECK: warning: use of moved-from value 'source'
// CHECK: warning: use of moved-from value 'value'
// CHECK: warning: use of moved-from value 'value'
// CHECK-NOT: warning:
// CHECK-NOT: error:
