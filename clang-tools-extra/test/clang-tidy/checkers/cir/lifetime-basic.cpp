// RUN: %check_clang_tidy %s cir-lifetime-check %t -- --export-fixes=%t.yaml -- -target x86_64-unknown-linux-gnu
// RUN: FileCheck %s --input-file=%t.yaml --check-prefix=CHECK-YAML

void dangling() {
  int *p;
  {
    int x = 0;
    p = &x;
    *p = 42;
  }
  *p = 42;
  // CHECK-NOTES: :[[@LINE-1]]:4: warning: use of invalid pointer 'p' [cir-lifetime-check]
  // CHECK-NOTES: :[[@LINE-3]]:3: note: pointee 'x' invalidated at end of scope
}

// CHECK-YAML: DiagnosticName:  cir-lifetime-check
// CHECK-YAML-NEXT: DiagnosticMessage:
// CHECK-YAML-NEXT: Message:         'use of invalid pointer ''p'''
// CHECK-YAML-NEXT: FilePath:        '{{.*}}.cpp'
// CHECK-YAML-NEXT: FileOffset:      {{[1-9][0-9]*}}
// CHECK-YAML-NEXT: Replacements:    []
// CHECK-YAML-NEXT: Notes:
// CHECK-YAML-NEXT: - Message:         'pointee ''x'' invalidated at end of scope'
// CHECK-YAML-NEXT: FilePath:        '{{.*}}.cpp'
// CHECK-YAML-NEXT: FileOffset:      {{[1-9][0-9]*}}
// CHECK-YAML-NEXT: Replacements:    []
// CHECK-YAML-NEXT: Level:           Warning
