// RUN: not clang-tidy %s --checks='-*,cir-lifetime-check' --config='{}' --export-fixes=%t.yaml -- -target x86_64-unknown-linux-gnu -std=c++20 2>&1 | FileCheck %s --implicit-check-not='PLEASE submit a bug report'
// RUN: FileCheck %s --input-file=%t.yaml --check-prefix=YAML

struct Resource {
  Resource();
  ~Resource();
  explicit operator bool() const;
};

// A CIR-generation failure inside a compound statement must reach clang-tidy
// as an error, without retrying the statement or running lifetime analysis.
void nested(bool condition) {
  if (condition) {
    while (const Resource &value = Resource()) {}
  }
}

// CHECK: error: ClangIR code gen Not Yet Implemented: loop condition with a lifetime-extended temporary requiring destruction [clang-diagnostic-error]
// YAML: DiagnosticName:  clang-diagnostic-error
// YAML: Message:{{.*}}loop condition with a lifetime-extended temporary requiring destruction
// YAML: Level:           Error
