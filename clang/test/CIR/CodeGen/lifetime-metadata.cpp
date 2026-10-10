// RUN: %clang_cc1 -std=c++20 -triple x86_64-unknown-linux-gnu -fclangir -emit-cir %s -o %t.cir
// RUN: FileCheck %s --input-file=%t.cir
// RUN: cir-opt %t.cir --verify-roundtrip | FileCheck %s

struct [[gsl::Owner]] Owner {
  int value;
  Owner();
  void mutate();
  int observe() const;
  static void staticUse();
  int &operator*() const;
};

struct [[gsl::Pointer]] Pointer {
  int *value;
};

// Incomplete types have lifetime facts even though they have no layout yet.
struct [[gsl::Owner]] Forward;
void incomplete(Forward *) {}

// Completing a previously converted record must refresh its lifetime facts.
struct Later;
void beforeDefinition(Later *) {}
struct [[gsl::Owner]] Later {};
void afterDefinition(Later) {}

// The final definition must also be observed without a later conversion.
struct LateOnly;
void beforeOnlyDefinition(LateOnly *) {}
struct [[gsl::Owner]] LateOnly {};

// Sema infers Owner on standard library types; no explicit annotation here.
namespace std {
template <class T> struct vector {
  T *data;
};
} // namespace std
void inferred(std::vector<int>) {}

template <class T> struct Task {
  struct promise_type {};
};
void coroutineType(Task<int>) {}

Owner::Owner() : value(0) {}

void methods(Owner &owner, Pointer pointer) {
  owner.mutate();
  owner.observe();
  Owner::staticUse();
  *owner;
  auto lambda = [] {};
  lambda();
}

// CHECK-DAG: Forward = #cir.lifetime_record<owner = true, pointer = false, lambda = false, promise_type = false>
// CHECK-DAG: Later = #cir.lifetime_record<owner = true, pointer = false, lambda = false, promise_type = false>
// CHECK-DAG: LateOnly = #cir.lifetime_record<owner = true, pointer = false, lambda = false, promise_type = false>
// CHECK-DAG: Owner = #cir.lifetime_record<owner = true, pointer = false, lambda = false, promise_type = false>
// CHECK-DAG: Pointer = #cir.lifetime_record<owner = false, pointer = true, lambda = false, promise_type = false>
// CHECK-DAG: "std::vector<int>" = #cir.lifetime_record<owner = true, pointer = false, lambda = false, promise_type = false>
// CHECK-DAG: "Task<int>" = #cir.lifetime_record<owner = false, pointer = false, lambda = false, promise_type = true>
// CHECK-DAG: #cir.lifetime_record<owner = false, pointer = false, lambda = true, promise_type = false>

// Constructor identity and lifetime facts are independent attributes.
// CHECK-DAG: cir.func {{.*}}@_ZN5OwnerC2Ev{{.*}}func_info<#cir.cxx_ctor<{{.*}}> attributes {{.*}}lifetime = #cir.lifetime_function<is_method = true, is_static = false, is_const = false, is_overloaded_operator = false{{.*}}>
// CHECK-DAG: cir.func {{.*}}@_ZN5Owner6mutateEv{{.*}}attributes {{.*}}lifetime = #cir.lifetime_function<is_method = true, is_static = false, is_const = false, is_overloaded_operator = false{{.*}}>
// CHECK-DAG: cir.func {{.*}}@_ZNK5Owner7observeEv{{.*}}attributes {{.*}}lifetime = #cir.lifetime_function<is_method = true, is_static = false, is_const = true, is_overloaded_operator = false{{.*}}>
// CHECK-DAG: cir.func {{.*}}@_ZN5Owner9staticUseEv{{.*}}attributes {{.*}}lifetime = #cir.lifetime_function<is_method = true, is_static = true, is_const = false, is_overloaded_operator = false{{.*}}>
// CHECK-DAG: cir.func {{.*}}@_ZNK5OwnerdeEv{{.*}}attributes {{.*}}lifetime = #cir.lifetime_function<is_method = true, is_static = false, is_const = true, is_overloaded_operator = true{{.*}}>
