// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -std=c++20 -fclangir -fclangir-lifetime-check="history=all" -verify -emit-cir %s -o /dev/null
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -std=c++20 -fclangir -fclangir-lifetime-check -clangir-disable-passes -emit-cir %s -o %t.cir
// RUN: cir-opt %t.cir --cir-canonicalize --cir-lifetime-check="history=all" -o /dev/null 2>&1 | FileCheck %s
// CHECK: warning: use of invalid pointer 'borrowed'

namespace std {
template<class T> T &&move(T &value) { return static_cast<T &&>(value); }
template<class T> class [[gsl::Owner(T)]] unique_ptr {
public:
  explicit unique_ptr(T *) noexcept;
  unique_ptr(unique_ptr &&) noexcept;
  ~unique_ptr();
  T *get() const noexcept;
  T *release() noexcept;
  void reset(T * = nullptr) noexcept;
};
template<class T> class [[gsl::Owner(T)]] shared_ptr {
public:
  explicit shared_ptr(T *) noexcept;
  shared_ptr(const shared_ptr &) noexcept;
  shared_ptr(shared_ptr &&) noexcept;
  ~shared_ptr();
  shared_ptr &operator=(const shared_ptr &) noexcept;
  T *get() const noexcept;
  void reset() noexcept;
};
}

int unique_move_preserves_borrow() {
  std::unique_ptr<int> source(new int(1));
  int *borrowed = source.get();
  std::unique_ptr<int> destination(std::move(source));
  source.reset(new int(2));
  return *borrowed;
}

int unique_destination_reset_invalidates_borrow() {
  std::unique_ptr<int> source(new int(1));
  int *borrowed = source.get();
  std::unique_ptr<int> destination(std::move(source));
  destination.reset(); // expected-note {{invalidated by non-const use of owner type}}
  return *borrowed; // expected-warning {{use of invalid pointer 'borrowed'}}
}

int shared_copy_retains_borrow() {
  std::shared_ptr<int> source(new int(1));
  int *borrowed = source.get();
  std::shared_ptr<int> destination(source);
  source.reset();
  return *borrowed;
}

int shared_last_reset_invalidates_borrow() {
  std::shared_ptr<int> source(new int(1));
  int *borrowed = source.get();
  std::shared_ptr<int> destination(source);
  source.reset();
  destination.reset(); // expected-note {{invalidated by non-const use of owner type}}
  return *borrowed; // expected-warning {{use of invalid pointer 'borrowed'}}
}

int shared_self_copy_preserves_borrow() {
  std::shared_ptr<int> source(new int(1));
  int *borrowed = source.get();
  source = source;
  return *borrowed;
}

int release_detaches_borrow() {
  std::unique_ptr<int> source(new int(1));
  int *borrowed = source.get();
  int *released = source.release();
  source.reset(new int(2));
  int value = *borrowed;
  delete released;
  return value;
}

int shared_inner_holder_destruction_keeps_borrow() {
  std::shared_ptr<int> source(new int(1));
  int *borrowed = source.get();
  {
    std::shared_ptr<int> destination(source);
  }
  return *borrowed;
}

int unique_inner_holder_destruction_invalidates_borrow() {
  std::unique_ptr<int> source(new int(1));
  int *borrowed = source.get();
  {
    std::unique_ptr<int> destination(std::move(source));
  } // expected-note {{pointee 'destination' invalidated at end of scope}}
  return *borrowed; // expected-warning {{use of invalid pointer 'borrowed'}}
}

void consume_shared(std::shared_ptr<int> &&) noexcept;

int opaque_consumption_removes_guaranteed_holder() {
  std::shared_ptr<int> source(new int(1));
  int *borrowed = source.get();
  std::shared_ptr<int> destination(source);
  consume_shared(std::move(destination));
  source.reset(); // expected-note {{invalidated by non-const use of owner type}}
  return *borrowed; // expected-warning {{use of invalid pointer 'borrowed'}}
}

void inspect_owner_address(const void *) noexcept;

unsigned char owner_storage_is_distinct_from_its_resource() {
  std::unique_ptr<int> owner(new int(1));
  std::unique_ptr<int> *address = &owner;
  owner.reset();
  inspect_owner_address(address);
  address->reset(new int(2));
  return *reinterpret_cast<unsigned char *>(address);
}
