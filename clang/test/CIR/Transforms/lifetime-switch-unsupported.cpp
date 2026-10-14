// RUN: not %clang_cc1 -triple x86_64-unknown-linux-gnu -std=c++20 -fclangir -fclangir-lifetime-check="history=all" -emit-cir %s -DINCOMING -o /dev/null 2>&1 | FileCheck %s --check-prefix=CFG-ERROR --implicit-check-not=Assertion --implicit-check-not='see current operation'
// RUN: not %clang_cc1 -triple x86_64-unknown-linux-gnu -std=c++20 -fclangir -fclangir-lifetime-check="history=all" -emit-cir %s -DNESTED -o /dev/null 2>&1 | FileCheck %s --check-prefix=CFG-ERROR --implicit-check-not=Assertion --implicit-check-not='see current operation'

// Incoming jumps enter a nested case region without its switch-entry state;
// nested case labels require broader control-flow lowering. Keep both explicit
// failures. Former SKIP_STORE/BACKEDGE cases are now lifetime-switch-goto.cpp.
// CFG-ERROR: error: {{.*}}unsupported control flow in lifetime analysis

#if defined(INCOMING)
void f(int n, bool skip) {
  int value = 0;
  int *p = nullptr;
  if (skip) goto use;
  p = &value;
  switch (n) {
  case 0:
  use:
    *p = 1;
    break;
  default: break;
  }
}
#elif defined(NESTED)
// A nested case belongs to this switch but is not a direct child. Entering
// case 0 must carry the null state through the nested scope into case 1.
// Treating both cases as independent entries would miss this dereference.
void f(int n) {
  int value = 0;
  int *p = &value;
  switch (n) {
  case 0:
    p = nullptr;
    {
    case 1:
      *p = 1;
    }
    break;
  default: break;
  }
}
#endif
