// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -std=c++20 -fclangir -fclangir-lifetime-check="history=all" -verify -emit-cir %s -o /dev/null
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -std=c++20 -fclangir -fclangir-lifetime-check -clangir-disable-passes -emit-cir %s -o %t.cir
// RUN: cir-opt %t.cir --cir-canonicalize --cir-lifetime-check="history=all" -o /dev/null 2>&1 | FileCheck %s
// RUN: cir-opt %t.cir --emit-bytecode -o %t.bc
// RUN: cir-opt %t.bc --cir-canonicalize --cir-lifetime-check="history=all" -o /dev/null 2>&1 | FileCheck %s

namespace std {
template <class T> T &&move(T &value) { return static_cast<T &&>(value); }
template <class A, class B> struct tuple {
  tuple(A, B) noexcept;
  tuple(const tuple &) noexcept;
  tuple(tuple &&) noexcept;
  tuple &operator=(const tuple &) noexcept;
  template <class C, class D> tuple &operator=(const tuple<C, D> &) noexcept;
};
template <class A, class B> tuple<A &, B &> tie(A &, B &) noexcept;
template <class A, class B> tuple<A, B> make_tuple(A, B) noexcept;
struct ignore_t {
  template <class T> const ignore_t &operator=(const T &) const noexcept;
};
inline constexpr ignore_t ignore;
} // namespace std

int constructor_retains_null_payload() {
  int value = 1;
  int *destination = &value;
  int *empty = nullptr; // expected-note {{'nullptr' invalidated here}}
  std::tuple<int *, int> source(empty, 2);
  std::tie(destination, std::ignore) = source;
  return *destination; // expected-warning {{use of invalid pointer 'destination'}}
}

int constructor_snapshots_value() {
  int value = 1;
  int *destination = &value;
  int *empty = nullptr; // expected-note {{'nullptr' invalidated here}}
  std::tuple<int *, int> source(empty, 2);
  empty = &value;
  std::tie(destination, std::ignore) = source;
  return *destination; // expected-warning {{use of invalid pointer 'destination'}}
}

int later_source_change_does_not_taint_snapshot() {
  int value = 1;
  int *pointer = &value, *destination = &value;
  std::tuple<int *, int> source(pointer, 2);
  pointer = nullptr;
  std::tie(destination, std::ignore) = source;
  return *destination;
}

int make_tuple_retains_null_payload() {
  int value = 1;
  int *destination = &value;
  int *empty = nullptr; // expected-note {{'nullptr' invalidated here}}
  std::tie(destination, std::ignore) = std::make_tuple(empty, 2);
  return *destination; // expected-warning {{use of invalid pointer 'destination'}}
}

int copy_retains_value_payload() {
  int value = 1;
  int *destination = &value;
  int *empty = nullptr; // expected-note {{'nullptr' invalidated here}}
  std::tuple<int *, int> source(empty, 2);
  std::tuple<int *, int> copy(source);
  std::tie(destination, std::ignore) = copy;
  return *destination; // expected-warning {{use of invalid pointer 'destination'}}
}

int move_retains_value_payload() {
  int value = 1;
  int *destination = &value;
  int *empty = nullptr; // expected-note {{'nullptr' invalidated here}}
  std::tuple<int *, int> source(empty, 2);
  std::tuple<int *, int> moved(std::move(source));
  std::tie(destination, std::ignore) = moved;
  return *destination; // expected-warning {{use of invalid pointer 'destination'}}
}

struct Custom {
  Custom() noexcept;
  Custom(const Custom &) noexcept;
  Custom &operator=(const Custom &) noexcept { return *this; }
  void use() const noexcept;
};
void consume(Custom &&) noexcept;

void custom_assignment_does_not_prove_reinitialization() {
  Custom destination, replacement;
  consume(std::move(destination)); // expected-note {{moved here via std::move or rvalue reference}}
  std::tie(destination, std::ignore) = std::make_tuple(replacement, 2);
  destination.use(); // expected-warning {{use of moved-from value 'destination'}}
}

struct [[gsl::Owner(int)]] Owner {
  Owner() noexcept;
};
struct [[gsl::Pointer(int)]] Borrow {
  Borrow(const Owner &) noexcept;
  Borrow(const Borrow &) noexcept;
  Borrow &operator=(const Borrow &) noexcept { return *this; }
  void read() const noexcept;
};
void consume(Borrow &&) noexcept;

void custom_pointer_assignment_does_not_prove_reinitialization() {
  Owner owner;
  Borrow destination(owner), replacement(owner);
  consume(std::move(destination)); // expected-note {{moved here via std::move or rvalue reference}}
  std::tie(destination, std::ignore) = std::make_tuple(replacement, 2);
  destination.read(); // expected-warning {{use of invalid pointer 'destination'}}
}

void reference_elements_have_no_left_to_right_guarantee() {
  int live = 1;
  int *p = nullptr; // expected-note 2 {{'nullptr' invalidated here}}
  int *q = &live;
  // Either complete element assignment may run first. Both resulting values
  // can be null; this is neither a simultaneous swap nor a fixed-order copy.
  std::tie(p, q) = std::tie(q, p);
  (void)*p; // expected-warning {{use of invalid pointer 'p'}}
  (void)*q; // expected-warning {{use of invalid pointer 'q'}}
}

void duplicate_destination_has_both_possible_last_writes() {
  int live = 1;
  int *p = &live;
  int *empty = nullptr; // expected-note {{'nullptr' invalidated here}}
  std::tuple<int *, int *> source(empty, &live);
  std::tie(p, p) = source;
  (void)*p; // expected-warning {{use of invalid pointer 'p'}}
}

void aliased_nonnull_elements_remain_valid() {
  int first = 1, second = 2;
  int *p = &first, *q = &second;
  std::tie(p, q) = std::tie(q, p);
  (void)*p;
  (void)*q;
  std::tuple<int *, int *> source(&first, &second);
  std::tie(p, p) = source;
  (void)*p;
}

// CHECK: warning: use of invalid pointer 'destination'
// CHECK: warning: use of invalid pointer 'destination'
// CHECK: warning: use of invalid pointer 'destination'
// CHECK: warning: use of invalid pointer 'destination'
// CHECK: warning: use of invalid pointer 'destination'
// CHECK: warning: use of moved-from value 'destination'
// CHECK: warning: use of invalid pointer 'destination'
// CHECK: warning: use of invalid pointer 'p'
// CHECK: warning: use of invalid pointer 'q'
// CHECK: warning: use of invalid pointer 'p'
// CHECK-NOT: warning:
// CHECK-NOT: error:
