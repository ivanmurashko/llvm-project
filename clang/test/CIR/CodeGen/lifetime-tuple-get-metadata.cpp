// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -std=c++20 -fclangir -fclangir-lifetime-check -clangir-disable-passes -emit-cir %s -o %t.cir
// RUN: FileCheck %s --input-file=%t.cir
// RUN: cir-opt %t.cir --verify-roundtrip | FileCheck %s
// RUN: cir-opt %t.cir --emit-bytecode -o %t.bc
// RUN: cir-opt %t.bc | FileCheck %s
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -std=c++20 -fclangir -clangir-disable-passes -emit-cir %s -o %t.ordinary.cir
// RUN: FileCheck %s --check-prefix=ORDINARY --input-file=%t.ordinary.cir

#include "../Inputs/lifetime-tuple-access.h"

void access(std::pair<int, long> &pair, const std::pair<int, long> &constant,
            std::tuple<int, long, int &> &tuple) {
  (void)std::get<0>(pair);
  (void)std::get<1>(constant);
  (void)std::get<0>(std::move(pair));
  (void)std::get<1>(std::move(constant));
  (void)std::get<int>(pair);
  (void)std::get<long>(std::move(constant));
  (void)std::get<2>(std::move(tuple));
  (void)std::get<int &>(std::move(tuple));
}

namespace other {
template <unsigned I> int &&get(std::pair<int, long> &&) noexcept;
}
struct Unrelated {};
namespace std {
template <unsigned I> int &&get(Unrelated &&) noexcept;
// Same name and source family, but not the standard selected result type.
template <unsigned I> long &&get(pair<int, int> &&) noexcept;
}
void not_accessors(std::pair<int, long> &pair, Unrelated &other,
                   std::pair<int, int> &wrongResult) {
  (void)other::get<0>(std::move(pair));
  (void)std::get<0>(std::move(other));
  (void)std::get<0>(std::move(wrongResult));
}

// CHECK-DAG: @_ZSt3getILj0ESt4pairIilEER{{.*}}parameter_kinds = [1], parameter_indices = [0], library_function = tuple_get
// CHECK-DAG: @_ZSt3getILj1ESt4pairIilEERK{{.*}}parameter_kinds = [1], parameter_indices = [0], library_function = tuple_get
// CHECK-DAG: @_ZSt3getILj0ESt4pairIilEEO{{.*}}parameter_kinds = [2], parameter_indices = [0], library_function = tuple_get
// CHECK-DAG: @_ZSt3getILj1ESt4pairIilEEOK{{.*}}parameter_kinds = [2], parameter_indices = [0], library_function = tuple_get
// CHECK-DAG: @_ZSt3getIiSt4pairIilEERT_RT0_{{.*}}library_function = tuple_get
// CHECK-DAG: @_ZSt3getIlSt4pairIilEEOKT_OKT0_{{.*}}library_function = tuple_get
// CHECK-DAG: @_ZSt3getILj2ESt5tupleIJilRiEEEON{{.*}}library_function = tuple_get
// CHECK-DAG: @_ZSt3getIRiSt5tupleIJilS0_EEEOT_OT0_{{.*}}library_function = tuple_get
// CHECK-DAG: @_ZN5other3getILj0EEEOiOSt4pairIilE{{.*}}parameter_indices = [0]>
// CHECK-DAG: @_ZSt3getILj0EEOiO9Unrelated{{.*}}parameter_indices = [0]>
// CHECK-DAG: @_ZSt3getILj0EEOlOSt4pairIiiE{{.*}}parameter_indices = [0]>
// ORDINARY: module
// ORDINARY-NOT: library_function = tuple_get
