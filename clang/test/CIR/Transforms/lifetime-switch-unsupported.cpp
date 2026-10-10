// RUN: not %clang_cc1 -triple x86_64-unknown-linux-gnu -std=c++20 -fclangir -fclangir-lifetime-check="history=all" -emit-cir %s -DINCOMING -o /dev/null 2>&1 | FileCheck %s --check-prefix=CFG-ERROR --implicit-check-not=Assertion --implicit-check-not='see current operation'
// RUN: not %clang_cc1 -triple x86_64-unknown-linux-gnu -std=c++20 -fclangir -fclangir-lifetime-check="history=all" -emit-cir %s -DSKIP_STORE -o /dev/null 2>&1 | FileCheck %s --check-prefix=CFG-ERROR --implicit-check-not=Assertion --implicit-check-not='see current operation'
// RUN: not %clang_cc1 -triple x86_64-unknown-linux-gnu -std=c++20 -fclangir -fclangir-lifetime-check="history=all" -emit-cir %s -DBACKEDGE -o /dev/null 2>&1 | FileCheck %s --check-prefix=CFG-ERROR --implicit-check-not=Assertion --implicit-check-not='see current operation'
// RUN: not %clang_cc1 -triple x86_64-unknown-linux-gnu -std=c++20 -fclangir -fclangir-lifetime-check="history=all" -emit-cir %s -DNESTED -o /dev/null 2>&1 | FileCheck %s --check-prefix=CFG-ERROR --implicit-check-not=Assertion --implicit-check-not='see current operation'

// Linear source-order traversal would miss null dereferences along the goto
// paths. Reject these explicitly until switch case CFG analysis is supported.
// CFG-ERROR: error: {{.*}}unsupported control flow in lifetime analysis

#if defined(SKIP_STORE)
void f(int n, bool skip) {
  int value = 0;
  int *p = nullptr;
  switch (n) {
  case 0:
    if (skip) goto use;
    p = &value;
  use:
    *p = 1;
    break;
  default: break;
  }
}
#elif defined(BACKEDGE)
void f(int n, bool again) {
  int value = 0;
  int *p = &value;
  switch (n) {
  case 0:
  repeat:
    *p = 1;
    p = nullptr;
    if (again) goto repeat;
    break;
  default: break;
  }
}
#elif defined(INCOMING)
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
