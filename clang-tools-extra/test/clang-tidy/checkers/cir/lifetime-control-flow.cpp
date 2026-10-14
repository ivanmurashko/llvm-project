// RUN: not clang-tidy %s --checks='-*,cir-lifetime-check' --config='{}' --export-fixes=%t.yaml -- -target x86_64-unknown-linux-gnu -std=c++20 2>&1 | FileCheck %s --check-prefix=CHECK --implicit-check-not='see current operation'
// RUN: FileCheck %s --input-file=%t.yaml --check-prefix=YAML --implicit-check-not='see current operation'

// An unsupported switch must retain its source error without dumping its IR.
void unsupported_switch(int n) {
  switch (n) {
  case 0: {
  case 1:
    break;
  }
  default:
    break;
  }
}
// CHECK-DAG: :[[@LINE-9]]:3: error: unsupported control flow in lifetime analysis: switch requires a simple direct-case shell [cir-lifetime-check]
// CHECK-DAG: error: CIR lifetime analysis failed [cir-lifetime-check]
// CHECK-NOT: see current operation
// YAML: DiagnosticName:  cir-lifetime-check
// YAML: Message:{{.*}}unsupported control flow in lifetime analysis: switch requires a simple direct-case shell
// YAML: FilePath:{{.*}}lifetime-control-flow.cpp
// YAML: Level:           Error
