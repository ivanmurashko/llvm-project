// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -std=c++20 -fclangir -fclangir-lifetime-check="history=all" -verify -emit-cir %s -o /dev/null
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -std=c++20 -fclangir -fclangir-lifetime-check -clangir-disable-passes -emit-cir %s -o %t.cir
// RUN: cir-opt %t.cir --cir-canonicalize --cir-lifetime-check="history=all" -o /dev/null 2>&1 | FileCheck %s

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
struct ignore_t {};
inline constexpr ignore_t ignore;
} // namespace std

int shared_reference_copy_retains_resource() {
  std::shared_ptr<int> source(new int(1)), destination(new int(2));
  int *borrowed = source.get();
  std::tie(destination, std::ignore) = std::tie(source, std::ignore);
  source.reset();
  return *borrowed;
}

int repeated_shared_self_copy_retains_resource() {
  std::shared_ptr<int> source(new int(1));
  int *borrowed = source.get();
  std::tie(source, source) = std::tie(source, source);
  return *borrowed;
}

int shared_reference_last_reset_invalidates_resource() {
  std::shared_ptr<int> source(new int(1)), destination(new int(2));
  int *borrowed = source.get();
  std::tie(destination, std::ignore) = std::tie(source, std::ignore);
  source.reset();
  destination.reset(); // expected-note {{invalidated by non-const use of owner type}}
  return *borrowed; // expected-warning {{use of invalid pointer 'borrowed'}}
}

int shared_reference_assignment_releases_old_resource() {
  std::shared_ptr<int> source(new int(1)), destination(new int(2));
  int *borrowed = destination.get();
  std::tie(destination, std::ignore) = std::tie(source, std::ignore); // expected-note {{invalidated by non-const use of owner type}}
  return *borrowed; // expected-warning {{use of invalid pointer 'borrowed'}}
}

// CHECK: warning: use of invalid pointer 'borrowed'
// CHECK: warning: use of invalid pointer 'borrowed'
// CHECK-NOT: warning:
