// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -std=c++20 -fclangir -clangir-disable-passes -emit-cir %s -o %t.noeh.cir
// RUN: FileCheck %s --check-prefixes=COMMON,NOEH < %t.noeh.cir
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -std=c++20 -fclangir -fclangir-lifetime-check -clangir-disable-passes -emit-cir %s -o %t.lifetime.cir
// RUN: FileCheck %s --check-prefixes=COMMON,NOEH < %t.lifetime.cir
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -std=c++20 -fclangir -fexceptions -fcxx-exceptions -clangir-disable-passes -emit-cir %s -o %t.eh.cir
// RUN: FileCheck %s --check-prefixes=COMMON,EH < %t.eh.cir

struct Temporary {
  Temporary();
  ~Temporary();
};

struct Element {
  Element(const Temporary &);
  ~Element();
};

// Initializer temporaries leave normal cleanups above the partial-array and
// operator-delete EH cleanups. With exceptions disabled those EH stack entries
// are inert, but still need to be deactivated without disturbing the temporaries.
extern "C" Element *new_array() {
  return new Element[2]{Element(Temporary()), Element(Temporary())};
}

// COMMON-LABEL: cir.func {{.*}} @new_array()
// COMMON: cir.call @_ZN9TemporaryC1Ev
// COMMON: cir.call @_ZN7ElementC1ERK9Temporary
// COMMON: cir.call @_ZN9TemporaryC1Ev
// COMMON: cir.call @_ZN7ElementC1ERK9Temporary
// NOEH: } cleanup normal {
// EH: } cleanup all {
// COMMON: cir.call @_ZN9TemporaryD1Ev
// NOEH: } cleanup normal {
// EH: } cleanup all {
// COMMON: cir.call @_ZN9TemporaryD1Ev
// NOEH-NOT: cleanup eh
// NOEH-NOT: cir.call @_ZN7ElementD1Ev
// NOEH-NOT: cir.call @_Zda
// EH: } cleanup eh {
// EH: cir.if
// EH: cir.call @_ZN7ElementD1Ev
// EH: } cleanup eh {
// EH: cir.if
// EH: cir.call @_Zda
// COMMON: cir.return
