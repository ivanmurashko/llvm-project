// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -std=c++20 -fclangir -fclangir-lifetime-check="history=all" -verify -emit-cir %s -o /dev/null
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -std=c++20 -fclangir -fclangir-lifetime-check -clangir-disable-passes -emit-cir %s -o %t.cir
// RUN: cir-opt %t.cir --cir-canonicalize --cir-lifetime-check="history=all" -o /dev/null 2>&1 | FileCheck %s
// RUN: cir-opt %t.cir --emit-bytecode -o %t.bc
// RUN: cir-opt %t.bc --cir-canonicalize --cir-lifetime-check="history=all" -o /dev/null 2>&1 | FileCheck %s

// An unmodeled non-const member call, or an owner passed to an opaque call,
// can replace a holder's contents. Occupancy, payload, predicate, and
// co-ownership facts maintained by library contracts must not survive it.
namespace std {
template <class T> class [[gsl::Owner(T)]] unique_ptr {
public:
  unique_ptr() noexcept;
  explicit unique_ptr(T *) noexcept;
  void swap(unique_ptr &) noexcept;
  T &operator*() const noexcept;
};
template <class T> class [[gsl::Owner(T)]] optional {
public:
  optional() noexcept;
  void emplace(T) noexcept;
  bool has_value() const noexcept;
  T &operator*() noexcept;
};
// Not a GSL owner, as in the standard library itself.
template <class T> class shared_ptr {
public:
  explicit shared_ptr(T *) noexcept;
  shared_ptr(const shared_ptr &) noexcept;
  ~shared_ptr() noexcept;
  void swap(shared_ptr &) noexcept;
  void reset() noexcept;
  T *get() const noexcept;
  T &operator*() const noexcept;
};
} // namespace std

void fill(std::unique_ptr<int> &) noexcept;

int swap_fills_empty_owner() {
  std::unique_ptr<int> empty;
  std::unique_ptr<int> full(new int(1));
  empty.swap(full);
  return *empty;
}

int opaque_call_fills_empty_owner() {
  std::unique_ptr<int> empty;
  fill(empty);
  return *empty;
}

int emplace_engages_optional() {
  std::optional<int> value;
  value.emplace(1);
  return *value;
}

int stale_predicate_does_not_refine(std::optional<int> &value) {
  bool engaged = value.has_value();
  value.emplace(1);
  if (!engaged)
    return *value;
  return 0;
}

int swap_fills_empty_library_holder() {
  std::shared_ptr<int> empty(nullptr);
  std::shared_ptr<int> full(new int(1));
  empty.swap(full);
  return *empty;
}

// After the swap, the first holder no longer co-owns the resource borrowed
// from the second, so resetting the second can release it.
int swap_detaches_co_owner() {
  std::shared_ptr<int> first(new int(1));
  std::shared_ptr<int> second(first);
  int *borrowed = second.get();
  {
    std::shared_ptr<int> other(new int(2));
    first.swap(other);
  }
  second.reset(); // expected-note {{invalidated by non-const use of owner type}}
  return *borrowed; // expected-warning {{use of invalid pointer 'borrowed'}}
}

int copy_keeps_co_owner() {
  std::shared_ptr<int> first(new int(1));
  std::shared_ptr<int> second(first);
  int *borrowed = second.get();
  second.reset();
  return *borrowed;
}

struct [[gsl::Owner(int)]] IntOwner {
  IntOwner();
  void mutate();
};

struct [[gsl::Pointer(int)]] IntPointer {
  IntPointer(const IntOwner &);
  int read();
};

// A receiver that may name either owner mutates both candidates.
int may_alias_owner_receiver(bool choose) {
  IntOwner first, second;
  IntPointer view(first);
  IntOwner &target = choose ? first : second;
  target.mutate(); // expected-note {{invalidated by non-const use of owner type}}
  return view.read(); // expected-warning {{use of invalid pointer 'view'}}
}

int may_alias_pointer_receiver(bool choose) {
  IntOwner live;
  IntPointer first(live);
  IntPointer second(live);
  {
    IntOwner temporary;
    first = temporary;
  } // expected-note {{pointee 'temporary' invalidated at end of scope}}
  IntPointer &target = choose ? first : second;
  return target.read(); // expected-warning {{use of invalid pointer 'first'}}
}

// Each candidate either keeps its pointee or receives the assigned one.
int may_alias_pointer_assignment(bool choose) {
  IntOwner live;
  IntPointer first(live);
  IntPointer second(live);
  {
    IntOwner temporary;
    IntPointer &target = choose ? first : second;
    target = temporary;
  } // expected-note {{pointee 'temporary' invalidated at end of scope}}
  return first.read(); // expected-warning {{use of invalid pointer 'first'}}
}

// CHECK: warning: use of invalid pointer 'borrowed'
// CHECK: warning: use of invalid pointer 'view'
// CHECK: warning: use of invalid pointer 'first'
// CHECK: warning: use of invalid pointer 'first'
// CHECK-NOT: warning:
// CHECK-NOT: error:
