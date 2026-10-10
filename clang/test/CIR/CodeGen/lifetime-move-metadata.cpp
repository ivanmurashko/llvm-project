// RUN: %clang_cc1 -std=c++23 -triple x86_64-unknown-linux-gnu -fclangir -clangir-disable-passes -emit-cir %s -o %t.cir
// RUN: FileCheck %s --check-prefix=CIR --input-file=%t.cir
// RUN: cir-opt %t.cir --verify-roundtrip | FileCheck %s --check-prefix=CIR
// RUN: %clang_cc1 -std=c++23 -triple x86_64-unknown-linux-gnu -fclangir -emit-cir %s -o %t-lowered.cir
// RUN: FileCheck %s --check-prefix=LOWERED --input-file=%t-lowered.cir
// RUN: %clang_cc1 -std=c++23 -triple x86_64-unknown-linux-gnu -fclangir -emit-llvm -disable-llvm-passes %s -o %t.ll
// RUN: FileCheck %s --check-prefix=LLVM --input-file=%t.ll

struct Value { int n; };

void parameters(int, Value &, const Value &, Value &&, const Value &&) {}
void variadic(Value &&, ...) {}
void noParameters() {}

struct Methods {
  void member(Value &&, int &);
  static void staticMember(Value &&);
  void explicitObject(this Methods &, Value &&);
};
void Methods::member(Value &&, int &) {}
void Methods::staticMember(Value &&) {}
void Methods::explicitObject(this Methods &, Value &&) {}

// pass_object_size introduces an extra CIR argument after its source parameter.
void objectSize(void *const p __attribute__((pass_object_size(0))), Value &&) {}

namespace std {
template <class T> struct [[gsl::Owner]] unique_ptr {
  T *get() const;
  T *release();
  void reset();
  explicit operator bool() const;
  T &operator*() const;
};
template <class T> struct shared_ptr { T *p; };
} // namespace std

namespace other {
template <class T> struct [[gsl::Owner]] unique_ptr { T *p; };
} // namespace other

void smartPointers(std::unique_ptr<int> &p, std::shared_ptr<int> shared,
                   other::unique_ptr<int> other) {
  p.get();
  p.release();
  p.reset();
  (void)static_cast<bool>(p);
  *p;
}

struct Base { virtual ~Base(); };
struct Derived : virtual Base { Derived(Value &&); };
Derived::Derived(Value &&) {}

// ABI lowering inserts an sret argument and passes the aggregate indirectly.
// Lifetime argument indices must not survive that signature rewrite unchanged.
struct Big { long values[3]; };
Big abiReturn(Value &&, Big value) { return value; }

// CIR-DAG: "std::unique_ptr<int>" = #cir.lifetime_record<owner = true, pointer = false, lambda = false, promise_type = false, smart_pointer = true>
// Smart-pointer identity alone does not infer Owner for std::shared_ptr.
// CIR-DAG: "std::shared_ptr<int>" = #cir.lifetime_record<owner = false, pointer = false, lambda = false, promise_type = false, smart_pointer = true>
// CIR-DAG: "other::unique_ptr<int>" = #cir.lifetime_record<owner = true, pointer = false, lambda = false, promise_type = false>

// Parameter kinds: 0 = value, 1 = lvalue reference, 2 = rvalue reference.
// CIR-DAG: @_Z10parameters{{.*}}lifetime = #cir.lifetime_function<{{.*}}parameter_kinds = [0, 1, 1, 2, 2], parameter_indices = [0, 1, 2, 3, 4]>
// CIR-DAG: @_Z8variadic{{.*}}lifetime = #cir.lifetime_function<{{.*}}parameter_kinds = [2], parameter_indices = [0]>
// CIR-DAG: @_Z12noParametersv{{.*}}lifetime = #cir.lifetime_function<{{.*}}parameter_kinds = [], parameter_indices = []>
// CIR-DAG: @_ZN7Methods6member{{.*}}lifetime = #cir.lifetime_function<{{.*}}has_implicit_object = true, method_name = "member", parameter_kinds = [2, 1], parameter_indices = [1, 2]>
// CIR-DAG: @_ZN7Methods12staticMember{{.*}}lifetime = #cir.lifetime_function<is_method = true, is_static = true, is_const = false, is_overloaded_operator = false, method_name = "staticMember", parameter_kinds = [2], parameter_indices = [0]>
// CIR-DAG: @{{.*}}explicitObject{{.*}}lifetime = #cir.lifetime_function<is_method = true, is_static = false, is_const = false, is_overloaded_operator = false, method_name = "explicitObject", parameter_kinds = [1, 2], parameter_indices = [0, 1]>
// CIR-DAG: @_Z10objectSize{{.*}}lifetime = #cir.lifetime_function<{{.*}}parameter_kinds = [0, 2], parameter_indices = [0, 2]>
// CIR-DAG: @_ZNKSt10unique_ptrIiE3getEv{{.*}}method_name = "get"
// CIR-DAG: @_ZNSt10unique_ptrIiE7releaseEv{{.*}}method_name = "release"
// CIR-DAG: @_ZNSt10unique_ptrIiE5resetEv{{.*}}method_name = "reset"
// CIR-DAG: @_ZNKSt10unique_ptrIiEcvbEv{{.*}}method_name = "operator bool"
// CIR-DAG: @_ZNKSt10unique_ptrIiEdeEv{{.*}}method_name = "operator*"
// A structor with virtual bases can have hidden ABI arguments; leave its
// parameter mapping unavailable instead of guessing the VTT argument position.
// CIR-DAG: @_ZN7DerivedC2E{{.*}}lifetime = #cir.lifetime_function<is_method = true, is_static = false, is_const = false, is_overloaded_operator = false, has_implicit_object = true, method_name = "Derived">
// CIR-DAG: @_Z9abiReturn{{.*}}lifetime = #cir.lifetime_function<{{.*}}parameter_kinds = [2, 0], parameter_indices = [0, 1]>

// LLVM-LABEL: define {{.*}}@_Z9abiReturnO5Value3Big(
// LLVM-SAME: sret(%struct.Big)
// LLVM-SAME: byval(%struct.Big)

// LOWERED-LABEL: cir.func {{.*}}@_Z9abiReturnO5Value3Big(
// LOWERED-NOT: #cir.lifetime_function
// LOWERED: cir.return
