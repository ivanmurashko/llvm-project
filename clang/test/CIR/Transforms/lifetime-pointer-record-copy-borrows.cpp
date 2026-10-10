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
  void reset(T * = nullptr) noexcept;
};
template <class K, class V> class map {
public:
  struct iterator {};
  map() noexcept;
  pair<iterator, bool> try_emplace(const K &, V &&);
  void clear() noexcept;
};
}

struct [[gsl::Pointer(int)]] View {
  int *data;
  View(const std::unique_ptr<int> &) noexcept;
  unsigned size() const noexcept;
};

unsigned copied_borrow_remains_live() {
  std::unique_ptr<int> source(new int(1));
  View borrow(source);
  View copy = borrow;
  return copy.size();
}

unsigned copied_borrow_retains_resource_lifetime() {
  std::unique_ptr<int> source(new int(1));
  View borrow(source);
  View copy = borrow;
  source.reset(); // expected-note {{invalidated by non-const use of owner}}
  return copy.size(); // expected-warning {{use of invalid pointer 'copy'}}
}

// A cir.copy performed after insertion must retain the source's conditional
// resource identity, including when the destination has record type.
unsigned copied_borrow_preserves_failed_insertion() {
  std::map<int, std::unique_ptr<int>> values;
  std::unique_ptr<int> source(new int(1));
  View borrow(source);
  auto inserted = values.try_emplace(1, std::move(source));
  View copy = borrow;
  if (!inserted.second) {
    values.clear();
    return copy.size();
  }
  return 0;
}

unsigned copied_borrow_source_reset_invalidates() {
  std::map<int, std::unique_ptr<int>> values;
  std::unique_ptr<int> source(new int(1));
  View borrow(source);
  auto inserted = values.try_emplace(1, std::move(source));
  View copy = borrow;
  if (!inserted.second) {
    source.reset(); // expected-note {{invalidated by non-const use of owner}}
    return copy.size(); // expected-warning {{use of invalid pointer 'copy'}}
  }
  return 0;
}

// Refinement cannot revive a resource already destroyed after the copy.
unsigned later_source_reset_remains_invalid() {
  std::map<int, std::unique_ptr<int>> values;
  std::unique_ptr<int> source(new int(1));
  View borrow(source);
  auto inserted = values.try_emplace(1, std::move(source));
  View copy = borrow;
  source.reset(); // expected-note {{invalidated by non-const use of owner}}
  if (!inserted.second)
    return copy.size(); // expected-warning {{use of invalid pointer 'copy'}}
  return 0;
}

// CHECK: warning: use of invalid pointer 'copy'
// CHECK: warning: use of invalid pointer 'copy'
// CHECK: warning: use of invalid pointer 'copy'
// CHECK-NOT: warning:
// CHECK-NOT: error:
