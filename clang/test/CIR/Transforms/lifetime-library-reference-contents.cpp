// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -std=c++20 -fclangir -fclangir-lifetime-check="history=all" -verify -emit-cir %s -o /dev/null
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -std=c++20 -fclangir -fclangir-lifetime-check -clangir-disable-passes -emit-cir %s -o %t.cir
// RUN: cir-opt %t.cir --cir-canonicalize --cir-lifetime-check="history=all" --verify-diagnostics -o /dev/null
// RUN: cir-opt %t.cir --emit-bytecode -o %t.bc
// RUN: cir-opt %t.bc --cir-lifetime-check="history=all" --verify-diagnostics -o /dev/null

#include "../Inputs/lifetime-tuple-access.h"

void take(std::tuple<int>) noexcept;
void take(std::tuple<int &>) noexcept;
void take(std::tuple<>) noexcept;

// The tuple's address is not its contents. Moving it into a fresh argument
// must not turn that argument into a pointer back to the moved source.
void incoming_values(std::tuple<int> &&source) {
  take(std::move(source));
}
void incoming_references(std::tuple<int &> &&source) {
  take(std::move(source));
}
void incoming_empty(std::tuple<> &&source) {
  take(std::move(source));
}
void local_references(int &value) {
  auto source = std::tie(value);
  take(std::move(source));
}
void repeated_move(std::tuple<int> &&source) {
  take(std::move(source)); // expected-note {{moved here via std::move or rvalue reference}}
  take(std::move(source)); // expected-warning {{use of moved-from value}}
}

namespace std {
template <class T> class [[gsl::Owner(T)]] unique_ptr {
public:
  explicit unique_ptr(T *) noexcept;
  unique_ptr(unique_ptr &&) noexcept;
  explicit operator bool() const noexcept;
  T &operator*() const noexcept;
};
}

int by_value_pointer_keeps_null_contents() {
  int *pointer = nullptr;
  std::unique_ptr<int> owner(pointer); // expected-note {{'nullptr' invalidated here}}
  return *owner; // expected-warning {{use of invalid pointer 'owner'}}
}

int known_null_survives_unknown_tuple(bool choice,
                                    std::tuple<int *> &incoming) {
  int live;
  int *empty = nullptr; // expected-note {{'nullptr' invalidated here}}
  std::tuple<int *> known(empty);
  std::tuple<int *> copy(choice ? known : incoming);
  int *pointer = &live;
  std::tie(pointer) = copy;
  return *pointer; // expected-warning {{use of invalid pointer 'pointer'}}
}

int unknown_tuple_does_not_prove_nonnull(bool choice,
                                       std::tuple<int *> &incoming) {
  int live;
  std::tuple<int *> known(&live);
  std::tuple<int *> copy(choice ? known : incoming);
  int *pointer = &live;
  std::tie(pointer) = copy;
  std::unique_ptr<int> owner(pointer);
  if (!owner)
    return *owner; // expected-warning {{use of invalid pointer 'owner'}}
  return 0;
}

void incoming_owner(std::unique_ptr<int> &&source) {
  std::unique_ptr<int> copy(std::move(source));
  (void)static_cast<bool>(copy);
}

int known_reference_alias_survives_unknown_tuple(
    bool choice, std::tuple<int *&> &incoming) {
  int live;
  int *empty = nullptr; // expected-note {{'nullptr' invalidated here}}
  auto known = std::tie(empty);
  std::tuple<int *&> copy(choice ? known : incoming);
  int *pointer = &live;
  std::tie(pointer) = copy;
  return *pointer; // expected-warning {{use of invalid pointer 'pointer'}}
}

int dangling_contents_survive_unknown_tuple(bool choice,
                                           std::tuple<int *> &incoming) {
  int *pointer = nullptr;
  std::tuple<int *> result(pointer);
  {
    int local = 1;
    std::tuple<int *> known(&local);
    result = choice ? known : incoming;
  } // expected-note {{pointee 'local' invalidated at end of scope}}
  std::tie(pointer) = result;
  return *pointer; // expected-warning {{use of invalid pointer 'pointer'}}
}

int *&unknown_pointer_reference() noexcept;
namespace std {
tuple<int *> make_tuple(int *&) noexcept;
}

// An untracked reference to a pointer needs unknown pointer contents. A
// pointerless summary would let a later assignment retain an old nonnull pset.
int unknown_pointer_contents_replace_old_value() {
  int live;
  int *pointer = &live;
  auto source = std::make_tuple(unknown_pointer_reference());
  std::tie(pointer) = source;
  std::unique_ptr<int> owner(pointer);
  if (!owner)
    return *owner; // expected-warning {{use of invalid pointer 'owner'}}
  return 0;
}
