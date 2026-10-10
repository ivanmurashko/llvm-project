// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -std=c++20 -fclangir -fclangir-lifetime-check="history=all" -verify -emit-cir %s -o %t.cir
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -std=c++20 -fclangir -fclangir-lifetime-check -clangir-disable-passes -emit-cir %s -o %t.raw.cir
// RUN: cir-opt %t.raw.cir --cir-canonicalize --cir-lifetime-check="history=all" -o /dev/null 2>&1 | FileCheck %s
// RUN: cir-opt %t.raw.cir --emit-bytecode -o %t.bc
// RUN: cir-opt %t.bc --cir-lifetime-check="history=all" -o /dev/null 2>&1 | FileCheck %s

namespace std {
template <class T> T &&move(T &value) { return static_cast<T &&>(value); }
// Logical pair elements need not be the only physical CIR members. A base
// precedes them and non-POD tail padding follows the boolean element.
struct pair_base { long marker; };
template <class A, class B> struct pair : pair_base { A first; B second; };
template <class T> class [[gsl::Owner(T)]] unique_ptr {
public:
  explicit unique_ptr(T *) noexcept;
  unique_ptr(unique_ptr &&) noexcept;
  explicit operator bool() const noexcept;
  T &operator*() const noexcept;
};
template <class K, class V> class map {
public:
  struct iterator { int position; bool second; };
  map() noexcept;
  pair<iterator, bool> try_emplace(const K &, V &&);
  iterator try_emplace(iterator, const K &, V &&);
  template <class A> pair<iterator, bool> try_emplace(const K &, A &&);
};
}

void insertion_success_consumes() {
  std::map<int, std::unique_ptr<int>> values;
  std::unique_ptr<int> source(new int(1));
  auto inserted = values.try_emplace(1, std::move(source)); // expected-note {{moved here via std::move or rvalue reference}}
  if (inserted.second)
    (void)*source; // expected-warning {{use of invalid pointer 'source'}}
}

void insertion_failure_preserves_source() {
  std::map<int, std::unique_ptr<int>> values;
  std::unique_ptr<int> source(new int(1));
  auto inserted = values.try_emplace(1, std::move(source));
  if (!inserted.second)
    (void)*source;
}

// The nested construction happens before try_emplace can reject the key.
void insertion_failure_does_not_undo_argument_effects() {
  std::map<int, std::unique_ptr<int>> values;
  std::unique_ptr<int> source(new int(1));
  auto inserted = values.try_emplace(1, std::unique_ptr<int>(std::move(source))); // expected-note {{moved here via std::move or rvalue reference}}
  if (!inserted.second)
    (void)*source; // expected-warning {{use of invalid pointer 'source'}}
}

void hinted_result_is_not_an_insertion_flag() {
  std::map<int, std::unique_ptr<int>> values;
  std::unique_ptr<int> source(new int(1));
  auto result = values.try_emplace({}, 1, std::move(source)); // expected-note {{moved here via std::move or rvalue reference}}
  if (!result.second)
    (void)*source; // expected-warning {{use of invalid pointer 'source'}}
}

struct InspectOnly {
  InspectOnly(std::unique_ptr<int> &&) noexcept;
};

void unknown_mapped_constructor_must_not_prove_null() {
  std::map<int, InspectOnly> values;
  std::unique_ptr<int> source(new int(1));
  int *bad = nullptr; // expected-note {{'nullptr' invalidated here}}
  auto result = values.try_emplace(1, std::move(source));
  // The mapped constructor can leave source nonempty. Pruning this branch
  // would hide an independently provable null dereference.
  if (result.second && source)
    (void)*bad; // expected-warning {{use of invalid pointer 'bad'}}
}

// CHECK: warning: use of invalid pointer 'source'
// CHECK: warning: use of invalid pointer 'source'
// CHECK: warning: use of invalid pointer 'source'
// CHECK: warning: use of invalid pointer 'bad'
