// RUN: %clang_cc1 -std=c++20 -triple x86_64-unknown-linux-gnu -fcxx-exceptions -fexceptions -fclangir -clangir-disable-passes -emit-cir %s -o - | FileCheck %s
// RUN: %clang_cc1 -std=c++20 -triple x86_64-unknown-linux-gnu -fcxx-exceptions -fexceptions -O3 -fclangir -clangir-disable-passes -emit-cir %s -o - | FileCheck %s
// RUN: %clang_cc1 -std=c++20 -triple x86_64-unknown-linux-gnu -fcxx-exceptions -fexceptions -fclangir -emit-llvm %s -o /dev/null
// RUN: %clang_cc1 -std=c++20 -triple x86_64-unknown-linux-gnu -fcxx-exceptions -fexceptions -O3 -fclangir -emit-llvm %s -o /dev/null
// RUN: %clang_cc1 -std=c++20 -triple x86_64-unknown-linux-gnu -fcxx-exceptions -fexceptions -fclangir -fclangir-lifetime-check -emit-llvm %s -o /dev/null
// RUN: %clang_cc1 -std=c++20 -triple x86_64-unknown-linux-gnu -fcxx-exceptions -fexceptions -O3 -fclangir -fclangir-lifetime-check -emit-llvm %s -o /dev/null

struct Element {
  ~Element();
};
struct Iterator {
  Element operator*();
  Iterator &operator++();
  bool operator!=(const Iterator &) const;
  ~Iterator();
};
struct Range {
  Iterator begin();
  Iterator end();
  ~Range();
};
struct Body {
  Body();
  ~Body();
};
Range make_range();
int action();
void observe(const Element &, const Body &);

// The range, begin, and end survive the iterations. The loop variable and
// body local share a nested per-iteration scope, including a non-compound
// source body. Cleanup runs on fallthrough, continue, break, return, and throw.
extern "C" void cleanup_exits() {
  for (Range init = make_range(); Element element : init) {
    Body body;
    if (action() == 1)
      continue;
    if (action() == 2)
      break;
    if (action() == 3)
      return;
    observe(element, body);
  }
}

// CHECK-LABEL: cir.func {{.*}}@cleanup_exits(
// CHECK: cir.scope {
// CHECK: %[[INIT:.*]] = cir.alloca "init"
// CHECK: %[[BEGIN:.*]] = cir.alloca "__begin1"
// CHECK: %[[END:.*]] = cir.alloca "__end1"
// CHECK-NOT: cir.alloca "element"
// CHECK-NOT: cir.alloca "body"
// CHECK: cir.for : cond {
// CHECK: } body {
// CHECK-NEXT: cir.scope {
// CHECK: %[[ELEMENT:.*]] = cir.alloca "element"
// CHECK: %[[BODY:.*]] = cir.alloca "body"
// CHECK: %[[VALUE:.*]] = cir.call @_ZN8IteratordeEv(%[[BEGIN]])
// CHECK: cir.store{{.*}} %[[VALUE]], %[[ELEMENT]]
// CHECK: cir.call @_ZN4BodyC1Ev(%[[BODY]])
// CHECK: cir.continue
// CHECK: cir.break
// CHECK: cir.return
// CHECK: cir.call @_Z7observeRK7ElementRK4Body(%[[ELEMENT]], %[[BODY]])
// CHECK: } cleanup all {
// CHECK: cir.call @_ZN4BodyD1Ev(%[[BODY]]) nothrow
// CHECK-NOT: cir.call @_ZN4BodyD1Ev
// CHECK: } cleanup all {
// CHECK: cir.call @_ZN7ElementD1Ev(%[[ELEMENT]]) nothrow
// CHECK-NOT: cir.call @_ZN7ElementD1Ev
// CHECK-NOT: cir.call @_ZN4BodyD1Ev
// CHECK: } step {
// CHECK: cir.call @_ZN8IteratorppEv(%[[BEGIN]])
// CHECK: cir.call @_ZN8IteratorD1Ev(%[[END]]) nothrow
// CHECK: cir.call @_ZN8IteratorD1Ev(%[[BEGIN]]) nothrow
// CHECK: cir.call @_ZN5RangeD1Ev(%[[INIT]]) nothrow

extern "C" void noncompound(Range &range) {
  for (Element element : range)
    (void)element;
}

// CHECK-LABEL: cir.func {{.*}}@noncompound(
// CHECK: cir.for : cond {
// CHECK: } body {
// CHECK-NEXT: cir.scope {
// CHECK: %[[ELEMENT:.*]] = cir.alloca "element"
// CHECK: %[[VALUE:.*]] = cir.call @_ZN8IteratordeEv(
// CHECK: cir.store{{.*}} %[[VALUE]], %[[ELEMENT]]
// CHECK: } cleanup all {
// CHECK: cir.call @_ZN7ElementD1Ev(%[[ELEMENT]]) nothrow
// CHECK-NOT: cir.call @_ZN7ElementD1Ev
// CHECK: } step {
