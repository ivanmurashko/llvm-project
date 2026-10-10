// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -std=c++17 -fclangir -fclangir-lifetime-check="history=all" -verify -emit-cir %s -o /dev/null

struct Pair { const int *first, *second; };
Pair make();

// A returned record replaces every tracked destination field with unknown
// contents. It does not leave the fields uninitialized or prove non-nullness.
int opaque_return() {
  Pair returned = make();
  return *returned.first;
}

int guarded_return() {
  Pair guarded = make();
  return guarded.first ? *guarded.first : 0;
}

int uninitialized_member() {
  Pair uninitialized; // expected-note {{uninitialized here}}
  return *uninitialized.first; // expected-warning {{use of invalid pointer 'uninitialized.first'}}
}

int null_member() {
  Pair null_value{nullptr, nullptr}; // expected-note {{'nullptr' invalidated here}}
  return *null_value.first; // expected-warning {{use of invalid pointer 'null_value.first'}}
}
