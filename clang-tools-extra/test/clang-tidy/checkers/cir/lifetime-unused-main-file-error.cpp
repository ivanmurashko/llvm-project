// RUN: clang-tidy %s --checks='-*,cir-lifetime-check' --config='{}' -- -target x86_64-unknown-linux-gnu -std=c++20 2>&1 | count 0
// RUN: not clang-tidy %s --checks='-*,cir-lifetime-check' --config='{CheckOptions: {cir-lifetime-check.AnalyzeUnusedMainFileFunctions: true}}' --export-fixes=%t.yaml -- -target x86_64-unknown-linux-gnu -std=c++20 2>&1 | FileCheck %s --implicit-check-not='PLEASE submit a bug report'
// RUN: FileCheck %s --input-file=%t.yaml --check-prefix=YAML

// A selected unsupported body is a generation failure, never a clean check.
inline void unsupported_selected_root(unsigned value) {
  (void)__builtin_elementwise_clzg(value);
}
// CHECK: error: ClangIR code gen Not Yet Implemented: __builtin_elementwise_clzg [clang-diagnostic-error]
// YAML: DiagnosticName:  clang-diagnostic-error
// YAML: Message:{{.*}}ClangIR code gen Not Yet Implemented: __builtin_elementwise_clzg
// YAML: Level:           Error
