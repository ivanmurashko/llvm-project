// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -std=c++14 -fclangir -fclangir-lifetime-check -clangir-disable-passes -emit-cir %s -o %t.14.cir
// RUN: FileCheck %s --implicit-check-not=cir.source_evaluation --implicit-check-not=cir.source_event < %t.14.cir
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -std=c++17 -fclangir -fclangir-lifetime-check -clangir-disable-passes -emit-cir %s -o %t.17.cir
// RUN: FileCheck %s --implicit-check-not=cir.source_evaluation --implicit-check-not=cir.source_event < %t.17.cir
// RUN: cir-opt %t.17.cir --verify-roundtrip -o %t.text.cir
// RUN: FileCheck %s --implicit-check-not=cir.source_evaluation --implicit-check-not=cir.source_event < %t.text.cir
// RUN: cir-opt %t.text.cir --emit-bytecode -o %t.bc
// RUN: cir-opt %t.bc --verify-roundtrip | FileCheck %s --implicit-check-not=cir.source_evaluation --implicit-check-not=cir.source_event
// RUN: cir-opt %t.17.cir --cir-canonicalize --verify-roundtrip -o /dev/null
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -std=c++17 -fclangir -clangir-disable-passes -emit-cir %s -o - | FileCheck %s --check-prefix=ORDINARY --implicit-check-not=lifetime_value_spill --implicit-check-not=cir.source_evaluation --implicit-check-not=cir.source_event

// Compiler-created storage carries expression values across cleanup scopes.
// The marker is independent of source-order scheduling and is emitted only
// when lifetime scope preservation is enabled.
struct Temporary {
  Temporary();
  ~Temporary();
  int value();
};

// CHECK-LABEL: cir.func{{.*}} @spill
// CHECK: cir.alloca "tmp.exprcleanup"{{.*}}lifetime_value_spill
// ORDINARY-LABEL: cir.func{{.*}} @spill
extern "C" int spill() {
  int value = Temporary().value();
  return value;
}

namespace std {
template <class T> T &&move(T &value) { return static_cast<T &&>(value); }
template <class T> T &&forward(T &value) { return static_cast<T &&>(value); }
}

extern "C" void consume(int, int);

// Value-category identities remain available without a source-event graph.
// CHECK-LABEL: cir.func{{.*}} @forwarding_helpers
// CHECK-NOT: cir.call @{{.*(move|forward)}}
// CHECK: cir.load xvalue
// CHECK-NOT: cir.call @{{.*(move|forward)}}
// CHECK: cir.call @consume
extern "C" void forwarding_helpers(int &value) {
  consume(std::move(value), std::forward<int &>(value));
}
