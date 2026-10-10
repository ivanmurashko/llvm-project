// RUN: not %clang_cc1 -triple x86_64-unknown-linux-gnu -std=c++20 -O0 -fclangir -emit-cir %s -o /dev/null 2>&1 | FileCheck %s --check-prefix=ERROR --implicit-check-not="Assertion" --implicit-check-not="fatal error:"
// RUN: not %clang_cc1 -triple x86_64-unknown-linux-gnu -std=c++20 -O3 -fclangir -emit-cir %s -o /dev/null 2>&1 | FileCheck %s --check-prefix=ERROR --implicit-check-not="Assertion" --implicit-check-not="fatal error:"
// RUN: not %clang_cc1 -triple x86_64-unknown-linux-gnu -std=c++20 -O0 -fclangir -fclangir-lifetime-check -emit-cir %s -o /dev/null 2>&1 | FileCheck %s --check-prefix=ERROR --implicit-check-not="Assertion" --implicit-check-not="fatal error:"
// RUN: not %clang_cc1 -triple x86_64-unknown-linux-gnu -std=c++20 -O3 -fclangir -fclangir-lifetime-check -emit-cir %s -o /dev/null 2>&1 | FileCheck %s --check-prefix=ERROR --implicit-check-not="Assertion" --implicit-check-not="fatal error:"
// RUN: not %clang_cc1 -triple x86_64-unknown-linux-gnu -std=c++20 -O0 -DTEST_FOR -fclangir -emit-cir %s -o /dev/null 2>&1 | FileCheck %s --check-prefix=ERROR --implicit-check-not="Assertion" --implicit-check-not="fatal error:"
// RUN: not %clang_cc1 -triple x86_64-unknown-linux-gnu -std=c++20 -O3 -DTEST_FOR -fclangir -emit-cir %s -o /dev/null 2>&1 | FileCheck %s --check-prefix=ERROR --implicit-check-not="Assertion" --implicit-check-not="fatal error:"
// RUN: not %clang_cc1 -triple x86_64-unknown-linux-gnu -std=c++20 -O0 -DTEST_FOR -fclangir -fclangir-lifetime-check -emit-cir %s -o /dev/null 2>&1 | FileCheck %s --check-prefix=ERROR --implicit-check-not="Assertion" --implicit-check-not="fatal error:"
// RUN: not %clang_cc1 -triple x86_64-unknown-linux-gnu -std=c++20 -O3 -DTEST_FOR -fclangir -fclangir-lifetime-check -emit-cir %s -o /dev/null 2>&1 | FileCheck %s --check-prefix=ERROR --implicit-check-not="Assertion" --implicit-check-not="fatal error:"
// RUN: not %clang_cc1 -triple x86_64-unknown-linux-gnu -std=c++20 -O0 -DTEST_AGGREGATE -fclangir -emit-cir %s -o /dev/null 2>&1 | FileCheck %s --check-prefix=ERROR --implicit-check-not="Assertion" --implicit-check-not="fatal error:"
// RUN: not %clang_cc1 -triple x86_64-unknown-linux-gnu -std=c++20 -O3 -DTEST_AGGREGATE -fclangir -fclangir-lifetime-check -emit-cir %s -o /dev/null 2>&1 | FileCheck %s --check-prefix=ERROR --implicit-check-not="Assertion" --implicit-check-not="fatal error:"
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -std=c++20 -O0 -DCONTROLS -fclangir -emit-cir %s -o /dev/null
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -std=c++20 -O3 -DCONTROLS -fclangir -emit-cir %s -o /dev/null
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -std=c++20 -O0 -DCONTROLS -fclangir -fclangir-lifetime-check -emit-cir %s -o /dev/null
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -std=c++20 -O3 -DCONTROLS -fclangir -fclangir-lifetime-check -emit-cir %s -o /dev/null

struct Resource {
  Resource();
  ~Resource();
  explicit operator bool() const;
};

struct Aggregate {
  const Resource &resource;
  explicit operator bool() const;
};

#ifndef CONTROLS
// Capturing this temporary's destructor into the loop cleanup requires a
// separate construction guard for exceptions during condition initialization.
// Reject before opening any loop state, rather than leaving cir.condition
// inside an ordinary cleanup scope or dropping the destructor.
void unsupported() {
#if defined(TEST_AGGREGATE)
  while (Aggregate value{Resource()}) {}
#elif defined(TEST_FOR)
  for (; const Resource &value = Resource();) {}
#else
  while (const Resource &value = Resource()) {}
#endif
}
// ERROR: error: ClangIR code gen Not Yet Implemented: loop condition with a lifetime-extended temporary requiring destruction
#else
void supported_lvalue_reference(Resource &resource) {
  while (const Resource &value = resource) { break; }
  for (; const Resource &value = resource;) { break; }
}

void supported_object_condition() {
  while (Resource value = Resource()) { break; }
}

// An extended scalar temporary has no destructor and needs no such guard.
void supported_trivial_temporary() {
  while (const int &value = 0) { break; }
}

struct Trivial {
  explicit operator bool() const;
};

void supported_trivial_class_temporary() {
  while (const Trivial &value = Trivial()) { break; }
}
#endif
