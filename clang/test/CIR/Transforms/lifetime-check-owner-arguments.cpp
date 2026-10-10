// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -std=c++20 -fclangir -fclangir-lifetime-check="history=all" -verify -emit-cir %s -o /dev/null
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -std=c++20 -fclangir -fclangir-lifetime-check -clangir-disable-passes -emit-cir %s -o %t.cir
// RUN: cir-opt %t.cir --cir-canonicalize --cir-lifetime-check="history=all" -o /dev/null 2>&1 | FileCheck %s
// RUN: cir-opt %t.cir --emit-bytecode -o %t.bc
// RUN: cir-opt %t.bc --cir-canonicalize --cir-lifetime-check="history=all" -o /dev/null 2>&1 | FileCheck %s

// An argument's storage is resolved through the path state, so an address
// that may denote several objects, or a standard-library holder outside the
// GSL categories, receives the owner and pointer effects of a call. Only a
// definite object is strongly updated.
namespace std {
template <class T> class [[gsl::Owner(T)]] optional {
public:
  optional(const T &) noexcept;
};
// Not a GSL owner, as in the standard library itself.
template <class T> class shared_ptr {
public:
  shared_ptr(T *) noexcept;
  shared_ptr(const shared_ptr &) noexcept;
  T *get() const noexcept;
  T &operator*() const noexcept;
};
} // namespace std

void mutate(std::shared_ptr<int> &) noexcept;

int holder_argument() {
  std::shared_ptr<int> owner(new int(1));
  int *borrow = owner.get();
  mutate(owner); // expected-note {{invalidated by non-const use of owner type}}
  return *borrow; // expected-warning {{use of invalid pointer 'borrow'}}
}

// A proven co-owner keeps the borrowed resource alive.
int holder_argument_with_co_owner() {
  std::shared_ptr<int> owner(new int(1));
  std::shared_ptr<int> keeper(owner);
  int *borrow = owner.get();
  mutate(owner);
  return *borrow;
}

int definite_holder_argument_forgets_emptiness() {
  std::shared_ptr<int> empty(nullptr);
  mutate(empty);
  return *empty;
}

// The other candidate may be unchanged, and remains possibly empty.
int may_alias_holder_argument(bool choose) {
  std::shared_ptr<int> first(nullptr); // expected-note {{'nullptr' invalidated here}}
  std::shared_ptr<int> second(nullptr);
  std::shared_ptr<int> *target = choose ? &first : &second;
  mutate(*target);
  return *first; // expected-warning {{use of moved-from value 'first'}}
}

struct [[gsl::Owner(int)]] Owner {
  Owner();
};

struct [[gsl::Pointer(int)]] View {
  View(Owner &);
  void use() const;
};

void mutate(Owner &);
void inspect(const View &);

void may_alias_owner_argument(bool choose) {
  Owner first, second;
  View borrow(first);
  Owner *target = choose ? &first : &second;
  mutate(*target); // expected-note {{invalidated by non-const use of owner type}}
  borrow.use(); // expected-warning {{use of invalid pointer 'borrow'}}
}

void may_alias_pointer_argument(bool choose) {
  Owner live;
  View first(live), second(live);
  {
    Owner temporary;
    first = temporary;
  } // expected-note {{pointee 'temporary' invalidated at end of scope}}
  View *target = choose ? &first : &second;
  inspect(*target); // expected-warning {{passing invalid pointer 'first'}}
}

void mutate(int *);

int scalar_argument_is_not_an_owner() {
  int value = 0;
  int *borrow = &value;
  mutate(&value);
  return *borrow;
}

// Unmodeled reference arguments of recognized contracts get the same checks.
struct Resource {
  Resource() noexcept;
  [[clang::reinitializes]] void restore(Owner &donor) noexcept;
  void use() const noexcept;
};

void reinitialization_mutates_owner_argument() {
  Resource target;
  Owner donor;
  View borrow(donor);
  target.restore(donor); // expected-note {{invalidated by non-const use of owner type}}
  borrow.use(); // expected-warning {{use of invalid pointer 'borrow'}}
}

void value_constructor_checks_pointer_argument() {
  Owner live;
  View view(live);
  {
    Owner temporary;
    view = temporary;
  } // expected-note {{pointee 'temporary' invalidated at end of scope}}
  std::optional<View> holder(view); // expected-warning {{passing invalid pointer 'view'}}
}

// CHECK: warning: use of invalid pointer 'borrow'
// CHECK: warning: use of moved-from value 'first'
// CHECK: warning: use of invalid pointer 'borrow'
// CHECK: warning: passing invalid pointer 'first'
// CHECK: warning: use of invalid pointer 'borrow'
// CHECK: warning: passing invalid pointer 'view'
// CHECK-NOT: warning:
// CHECK-NOT: error:

// A standard value constructor can copy an owner without mutating it. Keep
// its borrowed resource alive, while still checking pointer-like arguments.
void standard_copy_preserves_source_borrow() {
  Owner owner;
  View borrow(owner);
  std::optional<Owner> copy(owner);
  borrow.use();
}
