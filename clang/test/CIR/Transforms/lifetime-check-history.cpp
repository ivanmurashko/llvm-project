// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -std=c++20 -fclangir -fclangir-lifetime-check="history=all" -verify -emit-cir %s -o %t.cir

struct [[gsl::Owner(int)]] Owner {
  Owner();
  void mutate();
};

struct [[gsl::Pointer(int)]] Pointer {
  Pointer();
  Pointer(const Owner &);
  Pointer &operator=(const Pointer &);
  Pointer &operator=(Pointer &&);
  int read() const;
};

// Assignment must transfer the source's null origin along with its pointer
// state. A destination initialized from an owner has no null history of its own.
void copy_null_history() {
  Owner owner;
  Pointer dst(owner);
  Pointer src; // expected-note {{'nullptr' invalidated here}}
  dst = src;
  dst.read(); // expected-warning {{use of invalid pointer 'dst'}}
}

void move_null_history() {
  Owner owner;
  Pointer dst(owner);
  Pointer src; // expected-note {{'nullptr' invalidated here}}
  dst = static_cast<Pointer &&>(src);
  dst.read(); // expected-warning {{use of invalid pointer 'dst'}}
}

// The diagnostic must identify the source's invalidation, rather than the
// destination's earlier, transient uninitialized state.
void copy_invalid_history() {
  Owner sourceOwner;
  Pointer src(sourceOwner);
  Owner destinationOwner;
  Pointer dst(destinationOwner);
  sourceOwner.mutate(); // expected-note {{invalidated by non-const use of owner type}}
  dst = src;
  dst.read(); // expected-warning {{use of invalid pointer 'dst'}}
}

// Histories also describe states from other control-flow paths. Copying a valid
// pointer in one branch must preserve the origin needed by the joined state.
void preserve_null_history_at_join(bool flag) {
  Owner owner;
  Pointer dst; // expected-note {{'nullptr' invalidated here}}
  Pointer src(owner);
  if (flag)
    dst = src;
  dst.read(); // expected-warning {{use of invalid pointer 'dst'}}
}

void preserve_invalid_history_at_join(bool flag) {
  Owner owner;
  Pointer dst(owner);
  Pointer src(owner);
  if (flag)
    owner.mutate(); // expected-note {{invalidated by non-const use of owner type}}
  else
    dst = src;
  dst.read(); // expected-warning {{use of invalid pointer 'dst'}}
}
