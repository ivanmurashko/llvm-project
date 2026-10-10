// RUN: not %clang_cc1 -triple x86_64-unknown-linux-gnu -fclangir -fclangir-lifetime-check="yolo=invalid,null" -emit-cir %s -o - 2>&1 | FileCheck %s --implicit-check-not='see current operation'

// CHECK: invalid CIR lifetime-check options:
// RUN: not %clang_cc1 -triple x86_64-unknown-linux-gnu -fclangir -fclangir-lifetime-check="history=bogus" -emit-cir %s -o - 2>&1 | FileCheck %s --check-prefix=HISTORY --implicit-check-not='see current operation'
// RUN: not %clang_cc1 -triple x86_64-unknown-linux-gnu -fclangir -fclangir-lifetime-check="remarks=bogus" -emit-cir %s -o - 2>&1 | FileCheck %s --check-prefix=REMARKS --implicit-check-not='see current operation'
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -fclangir -clangir-disable-passes -emit-cir %s -o %t.cir
// RUN: not cir-opt %t.cir --cir-lifetime-check="history=bogus" -o /dev/null 2>&1 | FileCheck %s --check-prefix=HISTORY --implicit-check-not='see current operation'
// RUN: not cir-opt %t.cir --cir-lifetime-check="remarks=bogus" -o /dev/null 2>&1 | FileCheck %s --check-prefix=REMARKS --implicit-check-not='see current operation'

// HISTORY: invalid value 'bogus' for lifetime-check option 'history'
// REMARKS: invalid value 'bogus' for lifetime-check option 'remarks'
