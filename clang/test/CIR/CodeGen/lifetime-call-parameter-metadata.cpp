// RUN: %clang_cc1 -std=c++20 -triple x86_64-unknown-linux-gnu -fclangir -fclangir-lifetime-check -clangir-disable-passes -emit-cir %s -o %t.cir
// RUN: FileCheck %s --check-prefix=CIR --input-file=%t.cir
// RUN: cir-opt %t.cir --verify-roundtrip | FileCheck %s --check-prefix=CIR
// RUN: cir-opt %t.cir --cir-canonicalize --emit-bytecode -o %t.bc
// RUN: cir-opt %t.bc --verify-roundtrip | FileCheck %s --check-prefix=CIR
// RUN: %clang_cc1 -std=c++20 -triple x86_64-unknown-linux-gnu -fclangir -clangir-disable-passes -emit-cir %s -o - | FileCheck %s --check-prefix=ORDINARY --implicit-check-not=cir.lifetime_parameter_kind

using Mixed = void(int, int &, int &&);
extern "C" void mixed(int, int &, int &&);

// The indirect callee itself has no parameter-kind entry. Argument expressions
// have type int, but the source parameter types retain their reference kinds.
extern "C" void through_pointer(Mixed *callee, int &value) {
  callee(1, value, static_cast<int &&>(value));
}
// CIR-LABEL: cir.func{{.*}} @through_pointer(
// CIR: cir.call %{{.*}}({{.*}}) : (
// CIR-SAME: !s32i {cir.lifetime_parameter_kind = #cir.lifetime_parameter_kind<value>
// CIR-SAME: !cir.ptr<!s32i> {cir.lifetime_parameter_kind = #cir.lifetime_parameter_kind<lvalue_ref>
// CIR-SAME: !cir.ptr<!s32i> {cir.lifetime_parameter_kind = #cir.lifetime_parameter_kind<rvalue_ref>
// ORDINARY-LABEL: cir.func{{.*}} @through_pointer(

extern "C" void through_reference(Mixed &callee, int &value) {
  callee(2, value, static_cast<int &&>(value));
}
// CIR-LABEL: cir.func{{.*}} @through_reference(
// CIR: cir.call %{{.*}}({{.*}}) : (
// CIR-SAME: #cir.lifetime_parameter_kind<value>
// CIR-SAME: #cir.lifetime_parameter_kind<lvalue_ref>
// CIR-SAME: #cir.lifetime_parameter_kind<rvalue_ref>

extern "C" void through_direct(int &value) {
  mixed(3, value, static_cast<int &&>(value));
}
// CIR-LABEL: cir.func{{.*}} @through_direct(
// CIR-NOT: cir.lifetime_parameter_kind
// CIR: cir.call @mixed
// CIR-NOT: cir.lifetime_parameter_kind
// CIR: cir.return

// Reference-to-record and reference-to-function are first-class source binding
// types even though both become CIR pointers. No class-specific model is used.
struct Object { int value; };
using Callback = void();
using Bindings = void(Object &&, Callback &, Callback &&);
extern "C" void binding_types(Bindings *callee, Object &value, Callback &cb) {
  callee(static_cast<Object &&>(value), cb, static_cast<Callback &&>(cb));
}
// CIR-LABEL: cir.func{{.*}} @binding_types(
// CIR: cir.call %{{.*}}({{.*}}) : (
// CIR-SAME: #cir.lifetime_parameter_kind<rvalue_ref>
// CIR-SAME: #cir.lifetime_parameter_kind<lvalue_ref>
// CIR-SAME: #cir.lifetime_parameter_kind<rvalue_ref>

// No call metadata is invented for variadic or hidden-argument mappings.
using Variadic = void(int &&, ...);
extern "C" void variadic_excluded(Variadic *callee, int &value) {
  callee(static_cast<int &&>(value), 4);
}
// CIR-LABEL: cir.func{{.*}} @variadic_excluded(
// CIR-NOT: cir.lifetime_parameter_kind
// CIR: cir.return

void hidden(void *const __attribute__((pass_object_size(0))), int &&);
extern "C" void hidden_argument_excluded(int &value) {
  int array[2];
  hidden(array, static_cast<int &&>(value));
}
// CIR-LABEL: cir.func{{.*}} @hidden_argument_excluded(
// CIR-NOT: cir.lifetime_parameter_kind
// CIR: cir.return

// Instance-method calls retain their existing direct function metadata. F2
// does not infer a new call-site source mapping through their implicit object.
struct Method { void consume(int &&); };
extern "C" void method_excluded(Method &receiver, int &value) {
  receiver.consume(static_cast<int &&>(value));
}
// CIR-LABEL: cir.func{{.*}} @method_excluded(
// CIR-NOT: cir.lifetime_parameter_kind
// CIR: cir.return
