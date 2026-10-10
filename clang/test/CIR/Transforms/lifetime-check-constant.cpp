// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -std=c++20 -fclangir -fclangir-lifetime-check="history=all" -verify -emit-cir %s -o %t.cir
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -std=c++20 -fclangir -clangir-disable-passes -emit-cir %s -o %t.raw.cir
// RUN: cir-opt %t.raw.cir --cir-canonicalize --cir-lifetime-check="history=all" -o /dev/null 2>&1 | FileCheck %s --allow-empty
// CHECK-NOT: warning:
// CHECK-NOT: error:
// expected-no-diagnostics

constexpr int global = 42;
constexpr unsigned char signature[] = {1, 2, 3, 4};

const int *globalReference() {
  const int &reference = global;
  return &reference;
}

const unsigned char *arrayReference() {
  const auto &reference = signature;
  return reference;
}

struct Pointers {
  const int *global;
  const int *null;
};

int constantAggregate() {
  Pointers pointers{&global, nullptr};
  return *pointers.global;
}
