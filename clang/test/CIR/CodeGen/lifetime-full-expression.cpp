// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -std=c++20 -fclangir -fclangir-lifetime-check -clangir-disable-passes -emit-cir %s -o - | FileCheck %s --check-prefix=PRESERVE
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -std=c++20 -fclangir -clangir-disable-passes -emit-cir %s -o - | FileCheck %s --check-prefix=DEFAULT
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -std=c++20 -fclangir -fclangir-lifetime-check -verify -emit-cir %s -o /dev/null
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -std=c++20 -fclangir -fclangir-lifetime-check -verify -emit-llvm %s -o /dev/null

// expected-no-diagnostics

void consume(const int &);

// PRESERVE-LABEL: cir.func {{.*}} @_Z9temporaryv
// PRESERVE: cir.scope {
// PRESERVE: cir.alloca "ref.tmp
// PRESERVE: cir.call @_Z7consumeRKi
// PRESERVE: }
// PRESERVE: cir.return
// DEFAULT-LABEL: cir.func {{.*}} @_Z9temporaryv
// DEFAULT-NOT: cir.scope
// DEFAULT: cir.call @_Z7consumeRKi
// DEFAULT-NOT: cir.scope
// DEFAULT: cir.return
void temporary() { consume(1); }

// The materialized temporary belongs to the reference's enclosing scope.
// PRESERVE-LABEL: cir.func {{.*}} @_Z8extendedv
// PRESERVE: cir.alloca "ref.tmp
// PRESERVE: cir.alloca "value"
// PRESERVE: cir.scope {
// PRESERVE-NOT: cir.alloca "ref.tmp
// PRESERVE: }
// PRESERVE: cir.call @_Z7consumeRKi
int extended() {
  const int &value = 2;
  consume(value);
  return value;
}

struct Resource {
  Resource();
  ~Resource();
  int get() const;
};

// The scalar result is spilled before destruction and reloaded outside the
// full-expression region. The spill allocation must dominate both operations.
// PRESERVE-LABEL: cir.func {{.*}} @_Z13scalar_resultv
// PRESERVE: %[[SPILL:.*]] = cir.alloca "tmp.exprcleanup"
// PRESERVE: cir.scope {
// PRESERVE: %[[TEMP:.*]] = cir.alloca "ref.tmp
// PRESERVE: cir.call @_ZN8ResourceC1Ev(%[[TEMP]])
// PRESERVE: %[[RESULT:.*]] = cir.call @_ZNK8Resource3getEv(%[[TEMP]])
// PRESERVE: cir.store {{.*}} %[[RESULT]], %[[SPILL]]
// PRESERVE: } cleanup normal {
// PRESERVE: cir.call @_ZN8ResourceD1Ev(%[[TEMP]])
// PRESERVE: cir.yield
// PRESERVE-NEXT: }
// PRESERVE-NEXT: }
// PRESERVE: cir.load {{.*}} %[[SPILL]]
int scalar_result() {
  int value = Resource().get();
  return value;
}

// A conditional temporary must dominate its deferred cleanup without being
// moved outside the full-expression lifetime boundary.
// PRESERVE-LABEL: cir.func {{.*}} @_Z18conditional_resultb
// PRESERVE: cir.scope {
// PRESERVE: %[[CONDITIONAL_TEMP:.*]] = cir.alloca "ref.tmp
// PRESERVE: cir.ternary
// PRESERVE: cir.call @_ZN8ResourceC1Ev(%[[CONDITIONAL_TEMP]])
// PRESERVE: cir.call @_ZNK8Resource3getEv(%[[CONDITIONAL_TEMP]])
// PRESERVE: } cleanup normal {
// PRESERVE: cir.if
// PRESERVE: cir.call @_ZN8ResourceD1Ev(%[[CONDITIONAL_TEMP]])
int conditional_result(bool condition) {
  return condition ? Resource().get() : 0;
}

// Extending a non-trivial temporary must promote its destructor outside the
// initializer's full-expression region, preserving its use through the reference.
// PRESERVE-LABEL: cir.func {{.*}} @_Z17extended_resourcev
// PRESERVE: %[[EXTENDED_TEMP:.*]] = cir.alloca "ref.tmp
// PRESERVE: %[[REFERENCE_SPILL:.*]] = cir.alloca "tmp.exprcleanup"
// PRESERVE: cir.scope {
// PRESERVE-NOT: cir.alloca "ref.tmp
// PRESERVE: cir.call @_ZN8ResourceC1Ev(%[[EXTENDED_TEMP]])
// PRESERVE-NOT: cir.call @_ZN8ResourceD1Ev
// PRESERVE: cir.store {{.*}} %[[EXTENDED_TEMP]], %[[REFERENCE_SPILL]]
// PRESERVE: } cleanup normal {
// PRESERVE-NEXT: cir.yield
// PRESERVE-NEXT: }
// PRESERVE-NEXT: }
// PRESERVE-NEXT: cir.cleanup.scope {
// PRESERVE: cir.load {{.*}} %[[REFERENCE_SPILL]]
// PRESERVE: cir.call @_ZNK8Resource3getEv
// PRESERVE: } cleanup normal {
// PRESERVE: cir.call @_ZN8ResourceD1Ev(%[[EXTENDED_TEMP]])
int extended_resource() {
  const Resource &value = Resource();
  return value.get();
}
