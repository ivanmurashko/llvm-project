// RUN: %clang_cc1 -std=c++20 -triple x86_64-unknown-linux-gnu -fclangir -fclangir-lifetime-check -clangir-disable-passes -emit-cir %s -o %t.cir
// RUN: FileCheck %s --input-file=%t.cir
// RUN: cir-opt %t.cir --verify-roundtrip | FileCheck %s
// RUN: cir-opt %t.cir --emit-bytecode -o %t.bc
// RUN: cir-opt %t.bc | FileCheck %s

namespace std {
struct allocator_arg_t {};
template <class... Types> struct tuple {
  tuple(Types...) noexcept;
  tuple(const tuple &) noexcept;
  tuple(tuple &&) noexcept;
  tuple(allocator_arg_t, int, Types...) noexcept;
  template <class Other> tuple(Other) noexcept;
};
template <class T> struct remove_reference { using type = T; };
template <class T> struct remove_reference<T &> { using type = T; };
template <class T> struct remove_reference<T &&> { using type = T; };
template <class... Types>
tuple<typename remove_reference<Types>::type...>
make_tuple(Types &&...) noexcept;
template <class T> T &&move(T &value) { return static_cast<T &&>(value); }
} // namespace std

void tuples(int *pointer, int &reference) {
  std::tuple<int *, int> values(pointer, 1);
  std::tuple<int *, int &> references(pointer, reference);
  std::tuple<int *, int> copy(values);
  std::tuple<int *, int> moved(std::move(values));
  std::tuple<int *, int> allocated(std::allocator_arg_t{}, 0, pointer, 1);
  std::tuple<long> converted(1);
  auto result = std::make_tuple(pointer, 1);
}

// CHECK-DAG: library_kind = tuple, tuple_element_kinds = [0, 0]
// CHECK-DAG: library_kind = tuple, tuple_element_kinds = [0, 1]
// CHECK-DAG: method_name = "tuple", parameter_kinds = [0, 0], parameter_indices = [1, 2], library_function = construct, tuple_element_params = [0, 1]
// CHECK-DAG: method_name = "tuple", parameter_kinds = [0, 1], parameter_indices = [1, 2], library_function = construct, tuple_element_params = [0, 1]
// Copy/move use the existing special-member facts, not a one-element mapping.
// CHECK-DAG: method_name = "tuple", parameter_kinds = [1], parameter_indices = [1], library_function = construct>
// CHECK-DAG: method_name = "tuple", parameter_kinds = [2], parameter_indices = [1], library_function = construct>
// Allocator/tag and converting forms do not invent a logical argument mapping.
// CHECK-DAG: method_name = "tuple", parameter_kinds = [0, 0, 0, 0], parameter_indices = [1, 2, 3, 4], library_function = construct>
// CHECK-DAG: @_ZNSt5tupleIJlEEC1IiEET_{{.*}}library_function = construct>
// CHECK-DAG: is_method = false{{.*}}parameter_kinds = [1, 2], parameter_indices = [0, 1], library_function = make_tuple, tuple_element_params = [0, 1]
