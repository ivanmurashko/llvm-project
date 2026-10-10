// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -std=c++20 -fclangir -fclangir-lifetime-check="history=all" -verify -emit-cir %s -o /dev/null
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -std=c++20 -fclangir -fclangir-lifetime-check -clangir-disable-passes -emit-cir %s -o %t.cir
// RUN: cir-opt %t.cir --cir-canonicalize --cir-lifetime-check="history=all" -o /dev/null 2>&1 | FileCheck %s
// RUN: cir-opt %t.cir --emit-bytecode -o %t.bc
// RUN: cir-opt %t.bc --cir-lifetime-check="history=all" -o /dev/null 2>&1 | FileCheck %s

namespace std {
template <class T> T &&move(T &value) { return static_cast<T &&>(value); }
template <class A, class B> struct pair { A first; B second; };
template <class T> class [[gsl::Owner(T)]] unique_ptr {
public:
  explicit unique_ptr(T *) noexcept;
  unique_ptr(unique_ptr &&) noexcept;
  void reset(T * = nullptr) noexcept;
  T *get() const noexcept;
};
template <class K, class V> class map {
public:
  struct iterator {};
  map() noexcept;
  pair<iterator, bool> try_emplace(const K &, V &&);
  void clear() noexcept;
};
}

// Failure to insert preserves the source's ownership, including existing
// borrowers. Clearing an unrelated container cannot invalidate that borrow.
int failed_insertion_preserves_borrow() {
  std::map<int, std::unique_ptr<int>> values;
  std::unique_ptr<int> source(new int(1));
  int *borrow = source.get();
  auto inserted = values.try_emplace(1, std::move(source));
  if (!inserted.second) {
    values.clear();
    return *borrow;
  }
  return 0;
}

int failed_insertion_source_reset_invalidates() {
  std::map<int, std::unique_ptr<int>> values;
  std::unique_ptr<int> source(new int(1));
  int *borrow = source.get();
  auto inserted = values.try_emplace(1, std::move(source));
  if (!inserted.second) {
    source.reset(); // expected-note {{invalidated by non-const use of owner}}
    return *borrow; // expected-warning {{use of invalid pointer 'borrow'}}
  }
  return 0;
}

// Refining an older insertion result must not overwrite a completed later
// pointer assignment or restore a resource that has actually been destroyed.
int later_borrow_assignment_is_preserved() {
  std::map<int, std::unique_ptr<int>> values;
  std::unique_ptr<int> source(new int(1));
  int *borrow = source.get();
  auto inserted = values.try_emplace(1, std::move(source));
  int replacement = 2;
  borrow = &replacement;
  if (!inserted.second) {
    source.reset();
    values.clear();
    return *borrow;
  }
  return 0;
}

int later_source_reset_is_preserved() {
  std::map<int, std::unique_ptr<int>> values;
  std::unique_ptr<int> source(new int(1));
  int *borrow = source.get();
  auto inserted = values.try_emplace(1, std::move(source));
  source.reset(); // expected-note {{invalidated by non-const use of owner}}
  if (!inserted.second)
    return *borrow; // expected-warning {{use of invalid pointer 'borrow'}}
  return 0;
}

// Destruction on the insertion-success alternative cannot invalidate a
// borrower on the later-selected insertion-failure alternative.
int earlier_container_clear_is_conditional() {
  std::map<int, std::unique_ptr<int>> values;
  std::unique_ptr<int> source(new int(1));
  int *borrow = source.get();
  auto inserted = values.try_emplace(1, std::move(source));
  values.clear();
  if (!inserted.second)
    return *borrow;
  return 0;
}

// A pointer copied after insertion must carry the same conditional resource
// identity. It did not exist when the insertion's original state was saved.
int copied_borrow_preserves_failure_ownership() {
  std::map<int, std::unique_ptr<int>> values;
  std::unique_ptr<int> source(new int(1));
  int *borrow = source.get();
  auto inserted = values.try_emplace(1, std::move(source));
  int *copy = borrow;
  if (!inserted.second) {
    values.clear();
    return *copy;
  }
  return 0;
}

int copied_borrow_source_reset_invalidates() {
  std::map<int, std::unique_ptr<int>> values;
  std::unique_ptr<int> source(new int(1));
  int *borrow = source.get();
  auto inserted = values.try_emplace(1, std::move(source));
  int *copy = borrow;
  if (!inserted.second) {
    source.reset(); // expected-note {{invalidated by non-const use of owner}}
    return *copy; // expected-warning {{use of invalid pointer 'copy'}}
  }
  return 0;
}

// CHECK: warning: use of invalid pointer 'borrow'
// CHECK: warning: use of invalid pointer 'borrow'
// CHECK: warning: use of invalid pointer 'copy'
// CHECK-NOT: warning:
// CHECK-NOT: error:
