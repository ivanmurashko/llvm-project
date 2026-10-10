// RUN: %check_clang_tidy -std=c++20 %s cir-lifetime-check %t -- --export-fixes=%t.yaml -- -target x86_64-unknown-linux-gnu
// RUN: FileCheck %s --input-file=%t.yaml --check-prefix=CHECK-YAML

namespace std {
template <typename T> T &&move(T &value) { return static_cast<T &&>(value); }
}

void consume(int &&);

int use_after_move() {
  int value = 42;
  consume(std::move(value));
  return value;
  // CHECK-NOTES: :[[@LINE-1]]:{{[0-9]+}}: warning: use of moved-from value 'value' [cir-lifetime-check]
  // CHECK-NOTES: :[[@LINE-3]]:{{[0-9]+}}: note: moved here via std::move or rvalue reference
}

int ordinary_scalar_copy() {
  int source = 42;
  int copied = source;
  return source;
}

int explicit_scalar_move() {
  int source = 42;
  int moved = std::move(source);
  return source;
  // CHECK-NOTES: :[[@LINE-1]]:{{[0-9]+}}: warning: use of moved-from value 'source' [cir-lifetime-check]
  // CHECK-NOTES: :[[@LINE-3]]:{{[0-9]+}}: note: moved here via std::move or rvalue reference
}

// CHECK-YAML: DiagnosticName:  cir-lifetime-check
// CHECK-YAML-NEXT: DiagnosticMessage:
// CHECK-YAML-NEXT: Message:         'use of moved-from value ''value'''
// CHECK-YAML-NEXT: FilePath:        '{{.*}}.cpp'
// CHECK-YAML-NEXT: FileOffset:      {{[1-9][0-9]*}}
// CHECK-YAML-NEXT: Replacements:    []
// CHECK-YAML-NEXT: Notes:
// CHECK-YAML-NEXT: - Message:         'moved here via std::move or rvalue reference'
// CHECK-YAML-NEXT: FilePath:        '{{.*}}.cpp'
// CHECK-YAML-NEXT: FileOffset:      {{[1-9][0-9]*}}
// CHECK-YAML-NEXT: Replacements:    []
// CHECK-YAML-NEXT: Level:           Warning
