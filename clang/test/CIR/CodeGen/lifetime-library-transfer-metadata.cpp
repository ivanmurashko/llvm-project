// RUN: %clang_cc1 -std=c++20 -triple x86_64-unknown-linux-gnu -fclangir -fclangir-lifetime-check -clangir-disable-passes -emit-cir %s -o %t.cir
// RUN: FileCheck %s --input-file=%t.cir
// RUN: cir-opt %t.cir --verify-roundtrip | FileCheck %s
// RUN: cir-opt %t.cir --emit-bytecode -o %t.bc
// RUN: cir-opt %t.bc | FileCheck %s

namespace std {
template <class T> T &&move(T &value) { return static_cast<T &&>(value); }
template <class T> struct unique_ptr {};
template <class T> struct weak_ptr {};
template <class T> struct shared_ptr {
  shared_ptr(const shared_ptr &, T *) noexcept;
  shared_ptr(shared_ptr &&, T *) noexcept;
  shared_ptr(T *, int) noexcept;
};
struct pair_base { long marker; };
template <class A, class B> struct pair : pair_base { A first; B second; };
template <class Key, class Mapped> struct map {
  struct iterator { int position; };
  template <class... Args> pair<iterator, bool> try_emplace(const Key &, Args &&...);
  template <class... Args> pair<iterator, bool> try_emplace(Key &&, Args &&...);
  template <class... Args> iterator try_emplace(iterator, const Key &, Args &&...);
  Mapped &operator[](const Key &);
};
template <class T> struct vector { T &operator[](unsigned); };
}
struct Wrapper { Wrapper(std::unique_ptr<int> &&); };

void mapped_transfer(std::map<int, std::unique_ptr<int>> &values,
                     std::unique_ptr<int> &source, int &key) {
  values.try_emplace(key, std::move(source));
  values.try_emplace(std::map<int, std::unique_ptr<int>>::iterator{}, key,
                     std::move(source));
}
void key_transfer(std::map<std::unique_ptr<int>, int> &values,
                  std::unique_ptr<int> &source) {
  values.try_emplace(std::move(source), 1);
}
void unknown_constructor(std::map<int, Wrapper> &values,
                         std::unique_ptr<int> &source, int &key) {
  values.try_emplace(key, std::move(source));
}
void const_copy(std::map<long, std::shared_ptr<int>> &values,
                const std::shared_ptr<int> &source, long &key) {
  values.try_emplace(key, std::move(source));
}
void multiple_arguments(std::map<short, std::shared_ptr<int>> &values,
                        std::shared_ptr<int> &source, int *pointer, short &key) {
  values.try_emplace(key, std::move(source), pointer);
}
void aliases(std::shared_ptr<int> &source, int *pointer) {
  std::shared_ptr<int> copy(source, pointer);
  std::shared_ptr<int> transfer(std::move(source), pointer);
  std::shared_ptr<int> ordinary(pointer, 1);
}
int subscripts(std::map<int, int> &values, std::vector<int> &sequence, int &key) {
  return values[key] + sequence[0];
}

// CHECK-DAG: library_function = try_emplace, guaranteed_move_params = [1], insertion_result_index = 2
// CHECK-DAG: library_function = try_emplace_hint, guaranteed_move_params = [2]
// CHECK-DAG: library_function = try_emplace, guaranteed_move_params = [0], insertion_result_index = 2
// A custom mapped constructor, const source, or multi-argument construction
// does not establish a guaranteed transfer just because an argument is an &&.
// CHECK-DAG: @_ZNSt3mapIi7Wrapper{{.*}}library_function = try_emplace, insertion_result_index = 2>
// CHECK-DAG: @_ZNSt3mapIlSt10shared_ptr{{.*}}library_function = try_emplace, insertion_result_index = 2>
// CHECK-DAG: @_ZNSt3mapIsSt10shared_ptr{{.*}}library_function = try_emplace, insertion_result_index = 2>
// CHECK-DAG: method_name = "shared_ptr"{{.*}}parameter_kinds = [1, 0]{{.*}}library_function = shared_alias_construct
// CHECK-DAG: method_name = "shared_ptr"{{.*}}parameter_kinds = [2, 0]{{.*}}library_function = shared_alias_construct
// CHECK-DAG: method_name = "shared_ptr"{{.*}}parameter_kinds = [0, 0]{{.*}}library_function = construct
// CHECK-DAG: method_name = "operator[]"{{.*}}library_function = map_subscript
// CHECK-DAG: method_name = "operator[]"{{.*}}library_function = access
