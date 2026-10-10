// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -std=c++20 -fclangir -clangir-disable-passes -emit-cir %s -o /dev/null -verify
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -std=c++20 -fclangir -fclangir-lifetime-check -emit-cir %s -o /dev/null -verify
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -std=c++20 -fclangir -emit-llvm %s -o /dev/null -verify

namespace __cxxabiv1 {
struct __class_type_info;
}
struct Base {
  virtual ~Base();
};
struct Derived : Base {};

// CIR first declares the runtime with opaque type-info pointer parameters.
// A later typed runtime definition must report unsupported type replacement
// for cir.dyn_cast's symbol reference instead of aborting the compiler.
Derived *cast(Base *base) {
  return dynamic_cast<Derived *>(base); // expected-error@*:* {{ClangIR code gen Not Yet Implemented: function type replacement for a non-call symbol use}}
}

extern "C" const void *
__dynamic_cast(const void *object, const __cxxabiv1::__class_type_info *,
               const __cxxabiv1::__class_type_info *, __PTRDIFF_TYPE__) {
  return object;
}
