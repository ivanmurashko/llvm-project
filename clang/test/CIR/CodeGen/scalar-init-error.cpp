// RUN: not %clang_cc1 -triple x86_64-unknown-linux-gnu -std=c++17 -fblocks -fclangir -emit-cir %s -o /dev/null 2>&1 | FileCheck %s --implicit-check-not='PLEASE submit a bug report' --implicit-check-not='Assertion'
// RUN: not %clang_cc1 -triple x86_64-unknown-linux-gnu -std=c++17 -fblocks -fclangir -fclangir-lifetime-check -emit-cir %s -o /dev/null 2>&1 | FileCheck %s --implicit-check-not='PLEASE submit a bug report' --implicit-check-not='Assertion'
// RUN: not %clang_cc1 -triple x86_64-unknown-linux-gnu -std=c++17 -fblocks -fclangir -DSTATEMENT_EXPR -emit-cir %s -o /dev/null 2>&1 | FileCheck %s --implicit-check-not='PLEASE submit a bug report' --implicit-check-not='Assertion'
// RUN: not %clang_cc1 -triple x86_64-unknown-linux-gnu -std=c++17 -fblocks -fclangir -DSTATEMENT_EXPR -fclangir-lifetime-check -emit-cir %s -o /dev/null 2>&1 | FileCheck %s --implicit-check-not='PLEASE submit a bug report' --implicit-check-not='Assertion'
// RUN: not %clang_cc1 -triple x86_64-unknown-linux-gnu -std=c++17 -fblocks -fclangir -DCATCH_HANDLER -fcxx-exceptions -fexceptions -emit-cir %s -o /dev/null 2>&1 | FileCheck %s --implicit-check-not='PLEASE submit a bug report' --implicit-check-not='Assertion'
// RUN: not %clang_cc1 -triple x86_64-unknown-linux-gnu -std=c++17 -fblocks -fclangir -DCATCH_HANDLER -fcxx-exceptions -fexceptions -fclangir-lifetime-check -emit-cir %s -o /dev/null 2>&1 | FileCheck %s --implicit-check-not='PLEASE submit a bug report' --implicit-check-not='Assertion'

// A diagnosed scalar initializer can leave no CIR value. Do not store it,
// initialize later declarations from it, or emit subsequent uses of it.
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
// The final value bypasses emitStmt, so the enclosing compound must stop too.
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

// CHECK: error: ClangIR code gen Not Yet Implemented: processing of type: BlockPointer
// CHECK: error: ClangIR code gen Not Yet Implemented: ScalarExprEmitter: block
