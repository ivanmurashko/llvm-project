// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -std=c++20 -fclangir -fclangir-lifetime-check="history=all" -verify -emit-cir %s -o /dev/null
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -std=c++20 -fclangir -fclangir-lifetime-check -clangir-disable-passes -emit-cir %s -o %t.cir
// RUN: cir-opt %t.cir --cir-canonicalize --cir-lifetime-check="history=all" -o /dev/null 2>&1 | FileCheck %s
// RUN: cir-opt %t.cir --emit-bytecode -o %t.bc
// RUN: cir-opt %t.bc --cir-lifetime-check="history=all" -o /dev/null 2>&1 | FileCheck %s

namespace std {
template <class T> class [[gsl::Owner(T)]] unique_ptr {
public:
  explicit unique_ptr(T *) noexcept;
  void reset(T *) noexcept;
  explicit operator bool() const noexcept;
  T &operator*() const noexcept;
};
} // namespace std

// The incoming pointer has one symbolic referent, but no non-null guarantee.
int guarded_unknown_constructor(int *incoming) {
  std::unique_ptr<int> owner(incoming);
  if (!owner)
    return *owner; // expected-warning {{use of invalid pointer 'owner'}}
  return 0;
}

int guarded_unknown_reset(std::unique_ptr<int> &owner, int *incoming) {
  owner.reset(incoming);
  if (!owner)
    return *owner; // expected-warning {{use of invalid pointer 'owner'}}
  return 0;
}

// Unknown nullness alone is not a diagnosed null path. The symbolic identity
// is independent of whether the incoming pointer might be null.
int unguarded_unknown(std::unique_ptr<int> &owner, int *incoming) {
  owner.reset(incoming);
  return *owner;
}

void consume(int &&) noexcept;
int definite_write_through_copied_incoming_pointer(int *incoming) {
  consume(static_cast<int &&>(*incoming));
  int *alias = incoming;
  *alias = 3;
  return *incoming;
}

int address_of_reference_is_nonnull(int &value) {
  std::unique_ptr<int> owner(&value);
  if (!owner)
    return *owner;
  return 0;
}

int newly_allocated_pointer_is_nonnull() {
  std::unique_ptr<int> owner(new int(1));
  if (!owner)
    return *owner;
  return 0;
}

int copied_newly_allocated_pointer_is_nonnull() {
  int *allocated = new int(1);
  int *copy = allocated;
  std::unique_ptr<int> owner(copy);
  if (!owner)
    return *owner;
  return 0;
}

int *unknown_pointer() noexcept;
int unknown_alternative_survives_join(bool choice, int &value) {
  std::unique_ptr<int> owner(choice ? &value : unknown_pointer());
  if (!owner)
    return *owner; // expected-warning {{use of invalid pointer 'owner'}}
  return 0;
}

int known_null_alternative_survives_join(bool choice, int &value) {
  std::unique_ptr<int> owner(choice ? &value : nullptr); // expected-note {{'nullptr' invalidated here}}
  if (!owner)
    return *owner; // expected-warning {{use of invalid pointer 'owner'}}
  return 0;
}

int both_known_nonnull_alternatives(bool choice, int &first, int &second) {
  std::unique_ptr<int> owner(choice ? &first : &second);
  if (!owner)
    return *owner;
  return 0;
}

// CHECK: warning: use of invalid pointer 'owner'
// CHECK: warning: use of invalid pointer 'owner'
// CHECK: warning: use of invalid pointer 'owner'
// CHECK: warning: use of invalid pointer 'owner'
// CHECK-NOT: warning:
// CHECK-NOT: error:
