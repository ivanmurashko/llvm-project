// RUN: %clang_cc1 -fclangir -fclangir-lifetime-check -emit-cir %s -o /dev/null
// RUN: %clang_cc1 -fclangir -fclangir-lifetime-check=history=bogus -fclangir-lifetime-check -emit-cir %s -o /dev/null
// RUN: not %clang_cc1 -fclangir-lifetime-check -fsyntax-only %s 2>&1 | FileCheck %s --check-prefix=REQUIRES-CIR
// REQUIRES-CIR: '-fclangir-lifetime-check' only allowed with '-fclangir'

// RUN: %clang -### -fclangir -fclangir-lifetime-check -c %s 2>&1 | FileCheck %s --check-prefix=DEFAULT
// RUN: %clang -### -fclangir -fclangir-lifetime-check=history=all -c %s 2>&1 | FileCheck %s --check-prefix=OPTIONS
// RUN: %clang -### -fclangir -c %s 2>&1 | FileCheck %s --check-prefix=DISABLED

// DEFAULT: "-cc1"
// DEFAULT-SAME: "-fclangir-lifetime-check"
// OPTIONS: "-cc1"
// OPTIONS-SAME: "-fclangir-lifetime-check=history=all"
// DISABLED: "-cc1"
// DISABLED-NOT: -fclangir-lifetime-check

int main(void) { return 0; }
