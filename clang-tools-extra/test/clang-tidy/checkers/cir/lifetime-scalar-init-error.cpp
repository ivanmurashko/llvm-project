// RUN: not clang-tidy %s --checks='-*,cir-lifetime-check' --config='{}' --export-fixes=%t.yaml -- -target x86_64-unknown-linux-gnu -std=c++17 -fblocks 2>&1 | FileCheck %s --implicit-check-not='PLEASE submit a bug report' --implicit-check-not='Assertion'
// RUN: FileCheck %s --input-file=%t.yaml --check-prefix=YAML
// RUN: not clang-tidy %s --checks='-*,cir-lifetime-check' --config='{}' --export-fixes=%t.nested.yaml -- -target x86_64-unknown-linux-gnu -std=c++17 -fblocks -DSTATEMENT_EXPR 2>&1 | FileCheck %s --implicit-check-not='PLEASE submit a bug report' --implicit-check-not='Assertion'
// RUN: FileCheck %s --input-file=%t.nested.yaml --check-prefix=YAML
// RUN: not clang-tidy %s --checks='-*,cir-lifetime-check' --config='{}' --export-fixes=%t.catch.yaml -- -target x86_64-unknown-linux-gnu -std=c++17 -fblocks -DCATCH_HANDLER -fexceptions 2>&1 | FileCheck %s --implicit-check-not='PLEASE submit a bug report' --implicit-check-not='Assertion'
// RUN: FileCheck %s --input-file=%t.catch.yaml --check-prefix=YAML

// An unsupported initializer is a generation failure, even if later
// declarations and statements would use the object whose emission failed.
#if defined(CATCH_HANDLER)
void may_throw();
void initialize_block() {
  try {
    may_throw();
  } catch (...) {
    auto work = ^{};
    work();
  }
}
#elif defined(STATEMENT_EXPR)
int initialize_block() {
  return ({ auto work = ^{ return 1; }; work(); });
}
#else
void initialize_block() {
  auto work = ^{}, another = work;
  work();
  another();
  int later = 42;
  (void)later;
}
#endif

// CHECK: error: ClangIR code gen Not Yet Implemented: ScalarExprEmitter: block [clang-diagnostic-error]
// YAML: DiagnosticName:  clang-diagnostic-error
// YAML: Message:{{.*}}ClangIR code gen Not Yet Implemented: ScalarExprEmitter: block
// YAML: Level:           Error
