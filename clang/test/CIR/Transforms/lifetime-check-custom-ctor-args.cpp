// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -std=c++20 -fexceptions -fcxx-exceptions -fclangir -fclangir-lifetime-check="history=all" -verify -emit-cir %s -o /dev/null
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -std=c++20 -fexceptions -fcxx-exceptions -fclangir -fclangir-lifetime-check -clangir-disable-passes -emit-cir %s -o %t.cir
// RUN: cir-opt %t.cir --cir-canonicalize --cir-lifetime-check="history=all" -o /dev/null 2>&1 | FileCheck %s
// RUN: cir-opt %t.cir --emit-bytecode -o %t.bc
// RUN: cir-opt %t.bc --cir-canonicalize --cir-lifetime-check="history=all" -o /dev/null 2>&1 | FileCheck %s

// A custom constructor of an Owner or Pointer class passes its arguments like
// any other call. A Pointer constructed from an Owner still borrows it.
struct [[gsl::Owner(int)]] IntOwner {
  IntOwner();
  void mutate();
};

struct [[gsl::Pointer(int)]] IntPointer {
  IntPointer(const IntOwner &);
  IntPointer(const IntOwner &, const IntPointer &hint);
  IntPointer(const IntOwner &, IntOwner &mutated);
  int read();
};

struct [[gsl::Owner(int)]] Holder {
  Holder(IntOwner &donor);
  Holder(const IntPointer &view, int);
  Holder(int, IntOwner &donor);
};

void take(IntOwner &donor);
void take(const IntPointer &view, int);

int owner_argument_of_function() {
  IntOwner donor;
  IntPointer view(donor);
  take(donor); // expected-note {{invalidated by non-const use of owner type}}
  return view.read(); // expected-warning {{use of invalid pointer 'view'}}
}

int owner_argument_of_owner_constructor() {
  IntOwner donor;
  IntPointer view(donor);
  Holder holder(donor); // expected-note {{invalidated by non-const use of owner type}}
  return view.read(); // expected-warning {{use of invalid pointer 'view'}}
}

// The first argument does not name an object here.
int owner_argument_after_scalar() {
  IntOwner donor;
  IntPointer view(donor);
  Holder holder(0, donor); // expected-note {{invalidated by non-const use of owner type}}
  return view.read(); // expected-warning {{use of invalid pointer 'view'}}
}

void pointer_argument_of_function() {
  IntOwner live;
  IntPointer view(live);
  {
    IntOwner temporary;
    view = temporary;
  } // expected-note {{pointee 'temporary' invalidated at end of scope}}
  take(view, 0); // expected-warning {{passing invalid pointer 'view'}}
}

void pointer_argument_of_owner_constructor() {
  IntOwner live;
  IntPointer view(live);
  {
    IntOwner temporary;
    view = temporary;
  } // expected-note {{pointee 'temporary' invalidated at end of scope}}
  Holder holder(view, 0); // expected-warning {{passing invalid pointer 'view'}}
}

// Borrowing from an owner is not a non-const use of it.
int pointer_constructor_borrows_owner() {
  IntOwner owner;
  IntPointer first(owner);
  IntPointer second(owner);
  (void)first.read();
  (void)second.read();
  owner.mutate(); // expected-note 2 {{invalidated by non-const use of owner type}}
  (void)first.read(); // expected-warning {{use of invalid pointer 'first'}}
  return second.read(); // expected-warning {{use of invalid pointer 'second'}}
}

int pointer_constructor_checks_other_arguments() {
  IntOwner owner, live;
  IntPointer hint(live);
  {
    IntOwner temporary;
    hint = temporary;
  } // expected-note {{pointee 'temporary' invalidated at end of scope}}
  IntPointer view(owner, hint); // expected-warning {{passing invalid pointer 'hint'}}
  return view.read();
}

// A reference alias still binds the owner's contents. Both views survive the
// binding, and both become invalid when the actual owner later mutates.
void pointer_constructor_borrows_aliased_owner() {
  IntOwner owner;
  IntPointer first(owner);
  IntOwner &alias = owner;
  IntPointer second(alias);
  (void)first.read();
  (void)second.read();
  owner.mutate(); // expected-note 2 {{invalidated by non-const use of owner type}}
  (void)first.read(); // expected-warning {{use of invalid pointer 'first'}}
  (void)second.read(); // expected-warning {{use of invalid pointer 'second'}}
}

// The borrowing exemption belongs to the first explicit parameter, not the
// entire object. The second parameter can mutate that very same owner.
void pointer_constructor_repeated_owner_arguments() {
  IntOwner owner;
  IntPointer first(owner);
  IntPointer second(owner, owner); // expected-note 2 {{invalidated by non-const use of owner type}}
  (void)first.read(); // expected-warning {{use of invalid pointer 'first'}}
  (void)second.read(); // expected-warning {{use of invalid pointer 'second'}}
}

void owner_constructor_mutates_before_throw() {
  IntOwner owner;
  IntPointer view(owner);
  try {
    Holder holder(owner); // expected-note {{invalidated by non-const use of owner type}}
  } catch (...) {
    (void)view.read(); // expected-warning {{use of invalid pointer 'view'}}
  }
}

void borrowing_constructor_can_throw_without_mutation() {
  IntOwner owner;
  IntPointer first(owner);
  IntOwner &alias = owner;
  try {
    IntPointer second(alias);
  } catch (...) {
    (void)first.read();
  }
}

struct Resource {
  Resource() noexcept;
  void use() const noexcept;
};
struct Sink {
  Sink(Resource &&);
};
void consume(Resource &&) noexcept;

// The custom constructor's first argument is not a copy/move-special-member
// source. It receives the ordinary reference validation and consumption.
void custom_constructor_consumes_first_argument() {
  Resource source;
  Sink sink(static_cast<Resource &&>(source)); // expected-note {{moved here via std::move or rvalue reference}}
  source.use(); // expected-warning {{use of moved-from value 'source'}}
}

void custom_constructor_checks_first_argument() {
  Resource source;
  consume(static_cast<Resource &&>(source)); // expected-note {{moved here via std::move or rvalue reference}}
  Sink sink(static_cast<Resource &&>(source)); // expected-warning {{use of moved-from value 'source'}}
}

void custom_constructor_consumes_before_throw() {
  Resource source;
  try {
    Sink sink(static_cast<Resource &&>(source)); // expected-note {{moved here via std::move or rvalue reference}}
  } catch (...) {
    source.use(); // expected-warning {{use of moved-from value 'source'}}
  }
}

// CHECK: warning: use of invalid pointer 'view'
// CHECK: warning: use of invalid pointer 'view'
// CHECK: warning: use of invalid pointer 'view'
// CHECK: warning: passing invalid pointer 'view'
// CHECK: warning: passing invalid pointer 'view'
// CHECK: warning: use of invalid pointer 'first'
// CHECK: warning: use of invalid pointer 'second'
// CHECK: warning: passing invalid pointer 'hint'
// CHECK: warning: use of invalid pointer 'first'
// CHECK: warning: use of invalid pointer 'second'
// CHECK: warning: use of invalid pointer 'first'
// CHECK: warning: use of invalid pointer 'second'
// CHECK: warning: use of invalid pointer 'view'
// CHECK: warning: use of moved-from value 'source'
// CHECK: warning: use of moved-from value 'source'
// CHECK: warning: use of moved-from value 'source'
// CHECK-NOT: warning:
// CHECK-NOT: error:
