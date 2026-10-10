// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -std=c++17 -fclangir -fclangir-lifetime-check="history=all" -verify -emit-cir %s -o /dev/null
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -std=c++17 -fclangir -fclangir-lifetime-check -clangir-disable-passes -emit-cir %s -o %t.cir
// RUN: cir-opt %t.cir --cir-canonicalize --cir-lifetime-check="history=all" --verify-diagnostics -o /dev/null
// RUN: cir-opt %t.cir --cir-canonicalize --emit-bytecode -o %t.bc
// RUN: cir-opt %t.bc --cir-lifetime-check="history=all" --verify-diagnostics -o /dev/null

// A finite else-if chain, like X86TargetInfo::handleTargetFeatures, must not
// exhaust the native stack while recursively analyzing its CIR regions.
// Keep a diagnostic at the deepest leaf and an otherwise identical clean
// control, so skipping deep regions cannot make the regression pass.
#define STEP(N) if (key == (N)) return; else
#define EIGHT(N) \
  STEP((N) + 0) STEP((N) + 1) STEP((N) + 2) STEP((N) + 3) \
  STEP((N) + 4) STEP((N) + 5) STEP((N) + 6) STEP((N) + 7)
#define DEEP_CHAIN \
  EIGHT(0) EIGHT(8) EIGHT(16) EIGHT(24) EIGHT(32) EIGHT(40) \
  EIGHT(48) EIGHT(56) EIGHT(64) EIGHT(72) EIGHT(80) EIGHT(88)

void consume(int &&) noexcept;
void observe(int) noexcept;

void deep_invalid_use(int key) {
  int value = 1;
  consume(static_cast<int &&>(value)); // expected-note {{moved here via std::move or rvalue reference}}
  DEEP_CHAIN {
    observe(value); // expected-warning {{use of moved-from value 'value'}}
  }
}

void deep_valid_use(int key) {
  int value = 1;
  DEEP_CHAIN {
    observe(value);
  }
}
