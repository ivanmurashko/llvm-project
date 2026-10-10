// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -std=c++17 -fclangir -fclangir-lifetime-check="history=all" -verify -emit-cir %s -o /dev/null
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -std=c++17 -fclangir -fclangir-lifetime-check -clangir-disable-passes -emit-cir %s -o %t.cir
// RUN: cir-opt %t.cir --cir-canonicalize --cir-lifetime-check="history=all" --verify-diagnostics -o /dev/null
// RUN: cir-opt %t.cir --emit-bytecode -o %t.bc
// RUN: cir-opt %t.bc --cir-lifetime-check="history=all" --verify-diagnostics -o /dev/null

namespace std {
template <class A, class B> struct tuple {
  tuple(A, B) noexcept;
  template <class C, class D> tuple &operator=(tuple<C, D>) noexcept;
};
template <class A, class B> tuple<A &, B &> tie(A &, B &) noexcept;
struct ignore_t {};
inline constexpr ignore_t ignore;
} // namespace std

// This declaration-only contract model takes the source tuple by value. Its
// aggregate load must snapshot the completed construction, rather than the
// uninitialized compiler temporary. The null payload must reach the pointer.
int initialized_null_argument() {
  int value = 1;
  int *pointer = &value;
  std::tie(pointer, std::ignore) = std::tuple<int *, int>(nullptr, 2); // expected-note {{'nullptr' invalidated here}}
  return *pointer; // expected-warning {{use of invalid pointer 'pointer'}}
}

int initialized_valid_argument() {
  int value = 1;
  int *pointer = &value;
  std::tie(pointer, std::ignore) = std::tuple<int *, int>(&value, 2);
  return *pointer;
}
