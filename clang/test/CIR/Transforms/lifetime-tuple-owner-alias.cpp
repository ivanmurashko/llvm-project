// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -std=c++20 -fclangir -fclangir-lifetime-check="history=all" -verify -emit-cir %s -o /dev/null
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -std=c++20 -fclangir -fclangir-lifetime-check -clangir-disable-passes -emit-cir %s -o %t.cir
// RUN: cir-opt %t.cir --cir-canonicalize --cir-lifetime-check="history=all" -o /dev/null 2>&1 | FileCheck %s
// RUN: cir-opt %t.cir --emit-bytecode -o %t.bc
// RUN: cir-opt %t.bc --cir-lifetime-check="history=all" -o /dev/null 2>&1 | FileCheck %s

namespace std {
template <class T> class [[gsl::Owner(T)]] shared_ptr {
public:
  explicit shared_ptr(T *) noexcept;
  shared_ptr(const shared_ptr &) noexcept;
  ~shared_ptr();
  shared_ptr &operator=(const shared_ptr &) noexcept;
  void reset() noexcept;
  T *get() const noexcept;
};
template <class A, class B> struct tuple {
  template <class C, class D> tuple &operator=(const tuple<C, D> &) noexcept;
};
template <class A, class B> tuple<A &, B &> tie(A &, B &) noexcept;
}

// Replacing the only owner destroys the original resource regardless of the
// order of the tuple's element assignments. The pending raw-pointer snapshot
// must retain that destruction when it is subsequently stored into result.
int owner_then_raw_assignment() {
  std::shared_ptr<int> owner(new int(1)), replacement(new int(2));
  int *borrow = owner.get();
  int *result = nullptr;
  std::tie(owner, result) = std::tie(replacement, borrow); // expected-note {{invalidated by non-const use of owner}}
  return *result; // expected-warning {{use of invalid pointer 'result'}}
}

int raw_then_owner_assignment() {
  std::shared_ptr<int> owner(new int(1)), replacement(new int(2));
  int *borrow = owner.get();
  int *result = nullptr;
  std::tie(result, owner) = std::tie(borrow, replacement); // expected-note {{invalidated by non-const use of owner}}
  return *result; // expected-warning {{use of invalid pointer 'result'}}
}

int other_holder_preserves_original_resource() {
  std::shared_ptr<int> owner(new int(1)), replacement(new int(2));
  std::shared_ptr<int> holder(owner);
  int *borrow = owner.get();
  int *result = nullptr;
  std::tie(owner, result) = std::tie(replacement, borrow);
  return *result;
}

// Both assignments copy the same source into the same destination. Every
// permitted last write therefore leaves destination as a co-owner.
int duplicate_shared_copy_preserves_resource() {
  std::shared_ptr<int> source(new int(1)), destination(new int(2));
  int *borrow = source.get();
  std::tie(destination, destination) = std::tie(source, source);
  source.reset();
  return *borrow;
}

int duplicate_shared_copy_last_reset_invalidates() {
  std::shared_ptr<int> source(new int(1)), destination(new int(2));
  int *borrow = source.get();
  std::tie(destination, destination) = std::tie(source, source);
  source.reset();
  destination.reset(); // expected-note {{invalidated by non-const use of owner}}
  return *borrow; // expected-warning {{use of invalid pointer 'borrow'}}
}

// Distinct source objects are not a guaranteed common co-owner: the final
// element assignment may leave destination owning the other resource.
int duplicate_distinct_sources_are_not_definite_coowners() {
  std::shared_ptr<int> source(new int(1)), other(new int(2));
  std::shared_ptr<int> destination(new int(3));
  int *borrow = source.get();
  std::tie(destination, destination) = std::tie(source, other);
  source.reset(); // expected-note {{invalidated by non-const use of owner}}
  return *borrow; // expected-warning {{use of invalid pointer 'borrow'}}
}

// CHECK: warning: use of invalid pointer 'result'
// CHECK: warning: use of invalid pointer 'result'
// CHECK: warning: use of invalid pointer 'borrow'
// CHECK: warning: use of invalid pointer 'borrow'
// CHECK-NOT: warning:
// CHECK-NOT: error:
