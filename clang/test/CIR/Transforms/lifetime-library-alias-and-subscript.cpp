// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -std=c++20 -fclangir -fclangir-lifetime-check="history=all" -verify -emit-cir %s -o /dev/null
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -std=c++20 -fclangir -fclangir-lifetime-check -clangir-disable-passes -emit-cir %s -o %t.cir
// RUN: cir-opt %t.cir --cir-canonicalize --cir-lifetime-check="history=all" -o /dev/null 2>&1 | FileCheck %s
// RUN: cir-opt %t.cir --emit-bytecode -o %t.bc
// RUN: cir-opt %t.bc --cir-canonicalize --cir-lifetime-check="history=all" -o /dev/null 2>&1 | FileCheck %s

// These declaration-only fixtures describe standard-library contracts. The
// pointer stored by an aliasing shared_ptr is independent of its ownership.
namespace std {
template <class T> T &&move(T &value) { return static_cast<T &&>(value); }
template <class T> class [[gsl::Owner(T)]] shared_ptr {
public:
  explicit shared_ptr(T *) noexcept;
  shared_ptr(const shared_ptr &, T *) noexcept;
  shared_ptr(shared_ptr &&, T *) noexcept;
  ~shared_ptr() noexcept;
  void reset() noexcept;
  T *get() const noexcept;
  T &operator*() const noexcept;
};
template <class Key, class Mapped> class map {
public:
  map() noexcept;
  map(map &&) noexcept;
  Mapped &operator[](const Key &);
};
} // namespace std

int null_alias_dereference() {
  std::shared_ptr<int> owner(new int(1));
  std::shared_ptr<int> null_alias(owner, nullptr); // expected-note {{'nullptr' invalidated here}}
  return *null_alias; // expected-warning {{use of invalid pointer 'null_alias'}}
}

int null_alias_get() {
  std::shared_ptr<int> owner(new int(1));
  std::shared_ptr<int> null_alias(owner, nullptr); // expected-note {{'nullptr' invalidated here}}
  int *null_pointer = null_alias.get();
  return *null_pointer; // expected-warning {{use of invalid pointer 'null_pointer'}}
}

int alias_get_preserves_local_storage() {
  int local = 7;
  std::shared_ptr<int> owner(new int(1));
  std::shared_ptr<int> alias(owner, &local);
  int *borrowed = alias.get();
  alias.reset();
  owner.reset();
  return *borrowed;
}

int alias_dereference_preserves_local_storage() {
  int local = 7;
  std::shared_ptr<int> owner(new int(1));
  std::shared_ptr<int> alias(owner, &local);
  int &borrowed = *alias;
  alias.reset();
  owner.reset();
  return borrowed;
}

int null_alias_keeps_ownership() {
  std::shared_ptr<int> owner(new int(1));
  int *borrowed = owner.get();
  std::shared_ptr<int> alias(owner, nullptr);
  owner.reset();
  return *borrowed;
}

int moving_alias_keeps_local_storage() {
  int local = 7;
  std::shared_ptr<int> owner(new int(1));
  std::shared_ptr<int> alias(std::move(owner), &local);
  int *borrowed = alias.get();
  alias.reset();
  return *borrowed;
}

int empty_map_subscript_inserts() {
  std::map<int, int> values;
  return values[0];
}

int moved_map_subscript_inserts() {
  std::map<int, int> values;
  std::map<int, int> destination(std::move(values));
  return values[0];
}

int map_insertion_preserves_references() {
  std::map<int, int> values;
  int &existing = values[0];
  values[1] = 2;
  return existing;
}

// CHECK: warning: use of invalid pointer 'null_alias'
// CHECK: warning: use of invalid pointer 'null_pointer'
// CHECK-NOT: warning:
// CHECK-NOT: error:
