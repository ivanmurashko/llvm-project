// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -std=c++20 -I%S/Inputs -fclangir -fclangir-lifetime-check="history=all" -verify -emit-cir %s -o /dev/null
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -std=c++20 -I%S/Inputs -fclangir -fclangir-lifetime-check -clangir-disable-passes -emit-cir %s -o %t.cir
// RUN: cir-opt %t.cir --cir-canonicalize --cir-lifetime-check="history=all" -o /dev/null 2>&1 | FileCheck %s
// RUN: cir-opt %t.cir --emit-bytecode -o %t.bc
// RUN: cir-opt %t.bc --cir-lifetime-check="history=all" -o /dev/null 2>&1 | FileCheck %s

#include "std-cxx.h"

// Both arms create distinct, semantically equal observer predicates. Their
// join must retain the predicate even if the payloads have different addresses.
// Without refinement, incoming unknown nullness alone produces no warning.
int equal_root_predicates_survive_join(std::unique_ptr<int> &owner,
                                       bool choose) {
  bool present = choose ? static_cast<bool>(owner) : static_cast<bool>(owner);
  if (!present)
    return *owner; // expected-warning {{use of invalid pointer 'owner'}}
  return 0;
}

namespace std {
template <class K, class V> class map {
public:
  struct iterator {};
  map() noexcept;
  pair<iterator, bool> try_emplace(const K &, V &&);
  void clear() noexcept;
};
} // namespace std

// Negating a copied predicate must leave the original boolean's interpretation
// unchanged. Mutating a shared payload would make the final branch disappear.
int negation_preserves_original(std::unique_ptr<int> &owner) {
  int *bad = nullptr; // expected-note {{'nullptr' invalidated here}}
  bool present = static_cast<bool>(owner);
  bool absent = !present;
  if (absent)
    return 0;
  if (present)
    return *bad; // expected-warning {{use of invalid pointer 'bad'}}
  return 0;
}

// Extraction puts the insertion predicate into a saved scalar. The source
// reset invalidates snapshot facts in one branch. The other branch must retain
// its original snapshot so failed insertion still preserves the borrower.
int branch_mutation_preserves_other_snapshot(std::unique_ptr<int> &source,
                                             bool choose) {
  std::map<int, std::unique_ptr<int>> values;
  int *borrow = source.get();
  auto inserted = values.try_emplace(1, std::move(source));
  bool did_insert = inserted.second;
  if (choose) {
    source.reset(); // expected-note {{invalidated by non-const use of owner}}
    if (!did_insert)
      return *borrow; // expected-warning {{use of invalid pointer 'borrow'}}
    return 0;
  }
  if (!did_insert) {
    values.clear();
    return *borrow;
  }
  return 0;
}

// CHECK: warning: use of invalid pointer 'owner'
// CHECK: warning: use of invalid pointer 'bad'
// CHECK: warning: use of invalid pointer 'borrow'
// CHECK-NOT: warning:
// CHECK-NOT: error:
