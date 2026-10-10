// RUN: not clang-tidy %s --checks='-*,cir-lifetime-check' --config='{}' --export-fixes=%t.yaml -- -target x86_64-unknown-linux-gnu -std=c++20 2>&1 | FileCheck %s --implicit-check-not='PLEASE submit a bug report'
// RUN: FileCheck %s --input-file=%t.yaml --check-prefix=YAML

// A CIR runtime declaration with opaque type-info pointers can precede a
// source definition using typed pointers. Its unsupported non-call symbol use
// must remain a generation error, including through the clang-tidy adapter.
namespace __cxxabiv1 {
struct __class_type_info;
}
struct Base {
  virtual ~Base();
};
struct Derived : Base {};
Derived *cast(Base *base) { return dynamic_cast<Derived *>(base); }
extern "C" const void *
__dynamic_cast(const void *object, const __cxxabiv1::__class_type_info *,
               const __cxxabiv1::__class_type_info *, __PTRDIFF_TYPE__) {
  return object;
}

// CHECK: error: ClangIR code gen Not Yet Implemented: function type replacement for a non-call symbol use [clang-diagnostic-error]
// YAML: DiagnosticName:  clang-diagnostic-error
// YAML: Message:{{.*}}function type replacement for a non-call symbol use
// YAML: Level:           Error
