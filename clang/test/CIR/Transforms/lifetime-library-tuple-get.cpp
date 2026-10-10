// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -std=c++20 -fclangir -fclangir-lifetime-check="history=all" -verify=expected,clang -emit-cir %s -o /dev/null
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -std=c++20 -fclangir -fclangir-lifetime-check -clangir-disable-passes -emit-cir %s -o %t.cir
// RUN: cir-opt %t.cir --cir-canonicalize --cir-lifetime-check="history=all" --verify-diagnostics -o /dev/null
// RUN: cir-opt %t.cir --emit-bytecode -o %t.bc
// RUN: cir-opt %t.bc --cir-lifetime-check="history=all" --verify-diagnostics -o /dev/null

#include "../Inputs/lifetime-tuple-access.h"

std::pair<int, int> make_pair() noexcept;
std::tuple<int, int, int> make_tuple() noexcept;
void observe(int) noexcept;
void inspect(const std::pair<int, int> &) noexcept;
void consume(std::pair<int, int> &&) noexcept;

int returned_bindings() {
  auto [first, second] = make_pair();
  const auto [a, b, c] = make_tuple();
  return first + second + a + b + c;
}

int referenced_bindings(std::pair<int, int> &pair) {
  auto &[a, b] = pair;
  auto &&[c, d] = pair;
  const auto &[e, f] = pair;
  auto [g, h] = pair;
  return a + b + c + d + e + f + g + h;
}

void repeated_projections() {
  std::pair<int, long> pair(1, 2);
  (void)std::get<0>(std::move(pair));
  (void)std::get<1>(std::move(pair));
  (void)std::get<int>(std::move(pair));
  (void)std::get<long>(std::move(pair));
  const auto &constant = pair;
  (void)std::get<0>(std::move(constant));
  (void)std::get<long>(std::move(constant));
  observe(pair.first);
}

struct Pairs {
  std::pair<int, int> *begin() noexcept;
  std::pair<int, int> *end() noexcept;
};
void range_bindings(Pairs &pairs) {
  for (auto [first, second] : pairs)
    observe(first + second);
}

void accessor_does_not_restore_a_moved_source() {
  std::pair<int, int> pair(1, 2);
  consume(std::move(pair)); // expected-note {{moved here via std::move or rvalue reference}}
  // clang-note@-1 {{moved here via std::move or rvalue reference}}
  (void)std::get<0>(pair); // expected-warning {{use of moved-from value 'pair'}}
  (void)std::get<1>(std::move(pair)); // expected-warning {{use of moved-from value 'pair'}}
}

void projection_then_real_consumption() {
  std::pair<int, int> pair(1, 2);
  (void)std::get<0>(std::move(pair));
  consume(std::move(pair)); // expected-note {{moved here via std::move or rvalue reference}}
  inspect(pair); // expected-warning {{use of moved-from value 'pair'}}
}

void consume_element(int &&) noexcept;
void an_element_is_not_the_whole_pair() {
  std::pair<int, int> pair(1, 2);
  consume_element(std::get<0>(std::move(pair)));
  (void)std::get<1>(std::move(pair));
  inspect(pair);
}

namespace other {
template <unsigned I> int &&get(std::pair<int, int> &&) noexcept;
}
void unrelated_get_keeps_consuming_policy() {
  std::pair<int, int> pair(1, 2);
  (void)other::get<0>(std::move(pair)); // expected-note {{moved here via std::move or rvalue reference}}
  inspect(pair); // expected-warning {{use of moved-from value 'pair'}}
}

struct Plain { int first, second; };
int plain_aggregate_binding(Plain value) {
  auto [first, second] = value;
  return first + second;
}
