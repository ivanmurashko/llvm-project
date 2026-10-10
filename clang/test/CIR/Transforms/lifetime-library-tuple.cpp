// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -std=c++20 -fclangir -fclangir-lifetime-check="history=all" -verify -emit-cir %s -o %t.cir
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -std=c++20 -fclangir -fclangir-lifetime-check -clangir-disable-passes -emit-cir %s -o %t.raw.cir
// RUN: cir-opt %t.raw.cir --cir-canonicalize --cir-lifetime-check="history=all" -o /dev/null 2>&1 | FileCheck %s
// RUN: cir-opt %t.raw.cir --emit-bytecode -o %t.bc
// RUN: cir-opt %t.bc --cir-lifetime-check="history=all" -o /dev/null 2>&1 | FileCheck %s

namespace std {
template <class T> T &&move(T &value) { return static_cast<T &&>(value); }
template <class T, class U> struct tuple {
  T first;
  U second;
  template <class V, class W> tuple &operator=(const tuple<V, W> &) noexcept;
};
template <class T, class U> tuple<T &, U &> tie(T &, U &) noexcept;
struct ignore_t {
  template <class T> const ignore_t &operator=(const T &) const noexcept;
};
inline constexpr ignore_t ignore;
}
void consume(int &&) noexcept;
void observe(int) noexcept;

void bare_tie_is_not_a_write() {
  int source = 1, other = 2;
  consume(std::move(source)); // expected-note {{moved here via std::move or rvalue reference}}
  (void)std::tie(source, other);
  observe(source); // expected-warning {{use of moved-from value 'source'}}
}

void tie_assignment_reinitializes() {
  int destination = 1, other = 2, replacement = 3;
  consume(std::move(destination));
  std::tie(destination, std::ignore) = std::tie(replacement, other);
  observe(destination);
}

void tie_assignment_reads_source() {
  int source = 1, destination = 2;
  consume(std::move(source)); // expected-note {{moved here via std::move or rvalue reference}}
  std::tie(destination, std::ignore) = std::tie(source, destination); // expected-warning {{use of moved-from value 'source'}}
}

int tie_transfers_pointer_payload() {
  int value = 1, unused = 2;
  int *destination = &value;
  int *empty = nullptr; // expected-note {{'nullptr' invalidated here}}
  std::tie(destination, std::ignore) = std::tie(empty, unused);
  return *destination; // expected-warning {{use of invalid pointer 'destination'}}
}

// CHECK: warning: use of moved-from value 'source'
// CHECK: warning: use of moved-from value 'source'
// CHECK: warning: use of invalid pointer 'destination'
