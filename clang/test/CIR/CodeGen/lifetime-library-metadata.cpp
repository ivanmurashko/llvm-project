// RUN: %clang_cc1 -std=c++20 -triple x86_64-unknown-linux-gnu -fclangir -fclangir-lifetime-check -clangir-disable-passes -emit-cir %s -o %t.cir
// RUN: FileCheck %s --input-file=%t.cir
// RUN: cir-opt %t.cir --verify-roundtrip | FileCheck %s
// RUN: cir-opt %t.cir --emit-bytecode -o %t.bc
// RUN: cir-opt %t.bc | FileCheck %s
// RUN: %clang_cc1 -std=c++20 -triple x86_64-unknown-linux-gnu -fclangir -clangir-disable-passes -emit-cir %s -o - | FileCheck %s --check-prefix=DISABLED

namespace std {
inline namespace __test {
template <class T> class unique_ptr {
public:
  void reset(T * = nullptr) noexcept;
  T *get() const noexcept;
  explicit operator bool() const noexcept;
  T &operator*() const noexcept;
};
template <class T> class optional {
public:
  void reset() noexcept;
  bool has_value() const noexcept;
  T &operator*() & noexcept;
};
template <class T> class vector {
public:
  void clear() noexcept;
  void assign(unsigned, const T &);
  bool empty() const noexcept;
};
} // namespace __test
} // namespace std

struct Resource {
  [[clang::reinitializes]] void restore();
};
struct Lookalike {
  void reset(int *);
};

__attribute__((returns_nonnull)) int *nonnull_result();

void contracts(std::unique_ptr<int> &pointer, std::optional<int> &optional,
               std::vector<int> &container, Resource &resource,
               Lookalike &lookalike) {
  pointer.reset(nonnull_result());
  (void)pointer.get();
  (void)static_cast<bool>(pointer);
  (void)*pointer;
  optional.reset();
  (void)optional.has_value();
  (void)*optional;
  container.clear();
  container.assign(2, 1);
  (void)container.empty();
  resource.restore();
  lookalike.reset(nullptr);
}

// CHECK-DAG: "std::__test::unique_ptr<int>" = #cir.lifetime_record<{{.*}}library_kind = unique_ptr
// CHECK-DAG: "std::__test::optional<int>" = #cir.lifetime_record<{{.*}}library_kind = optional
// CHECK-DAG: "std::__test::vector<int>" = #cir.lifetime_record<{{.*}}library_kind = container
// CHECK-DAG: method_name = "reset"{{.*}}library_function = reset
// CHECK-DAG: method_name = "has_value"{{.*}}library_function = has_value
// CHECK-DAG: method_name = "clear"{{.*}}library_function = clear
// CHECK-DAG: method_name = "assign"{{.*}}library_function = assign
// CHECK-DAG: method_name = "restore"{{.*}}reinitializes = true
// CHECK-DAG: @_Z14nonnull_resultv(){{.*}}returns_nonnull = true
// CHECK-DAG: @_ZN9Lookalike5resetEPi{{.*}}method_name = "reset"{{[^>]*}}>
// DISABLED-NOT: library_kind
// DISABLED-NOT: library_function
// DISABLED-NOT: reinitializes
// DISABLED-NOT: returns_nonnull
