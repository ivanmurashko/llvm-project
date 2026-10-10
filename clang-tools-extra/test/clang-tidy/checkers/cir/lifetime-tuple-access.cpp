// RUN: %check_clang_tidy -std=c++20 %s cir-lifetime-check %t -- --export-fixes=%t.yaml -- -target x86_64-unknown-linux-gnu -I %S/../../../../../clang/test/CIR/Inputs
// RUN: FileCheck %s --input-file=%t.yaml --check-prefix=CHECK-YAML

#include "lifetime-tuple-access.h"

std::pair<int, int> make_pair() noexcept;
int valid_binding() {
  auto [first, second] = make_pair();
  return first + second;
}

void take(std::tuple<int &>) noexcept;
void valid_transfer(std::tuple<int &> &&source) {
  take(std::move(source));
}

void consume(std::pair<int, int> &&) noexcept;
void moved_source() {
  std::pair<int, int> source(1, 2);
  consume(std::move(source));
  (void)std::get<0>(source);
  // CHECK-NOTES: :[[@LINE-1]]:{{[0-9]+}}: warning: use of moved-from value 'source' [cir-lifetime-check]
  // CHECK-NOTES: :[[@LINE-3]]:{{[0-9]+}}: note: moved here via std::move or rvalue reference
}

// CHECK-YAML: DiagnosticName:  cir-lifetime-check
// CHECK-YAML: Message:         'use of moved-from value ''source'''
// CHECK-YAML: Message:         'moved here via std::move or rvalue reference'
// CHECK-YAML-NOT: DiagnosticName:
