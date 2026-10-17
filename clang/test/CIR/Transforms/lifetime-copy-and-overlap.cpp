// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -std=c++20 -fclangir -fclangir-lifetime-check="history=all" -verify -emit-cir %s -o /dev/null
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -std=c++20 -fclangir -fclangir-lifetime-check -clangir-disable-passes -emit-cir %s -o %t.cir
// RUN: cir-opt %t.cir --mlir-disable-threading --cir-lifetime-check="history=all" -o /dev/null 2> %t.serial
// RUN: FileCheck %s --input-file=%t.serial --implicit-check-not='warning:' --implicit-check-not='error:'
// RUN: cir-opt %t.cir --mlir-disable-threading=false --cir-lifetime-check="history=all" -o /dev/null 2> %t.parallel
// RUN: diff %t.serial %t.parallel
// RUN: cir-opt %t.cir --cir-canonicalize --mlir-print-debuginfo -o %t.text.cir
// RUN: cir-opt %t.text.cir --cir-lifetime-check="history=all" -o /dev/null 2> %t.text
// RUN: diff %t.serial %t.text
// RUN: cir-opt %t.text.cir --emit-bytecode -o %t.bc
// RUN: cir-opt %t.bc --cir-lifetime-check="history=all" -o /dev/null 2> %t.bytecode
// RUN: diff %t.serial %t.bytecode

namespace std {
template <class T> T &&move(T &v) { return static_cast<T &&>(v); }
template <class T> struct [[gsl::Owner(T)]] unique_ptr {
  explicit unique_ptr(T *) noexcept;
  unique_ptr(unique_ptr &&) noexcept;
  unique_ptr &operator=(unique_ptr &&) noexcept;
  T *get() const noexcept;
  T &operator*() const noexcept;
};
template <class T> struct vector {
  explicit vector(unsigned) noexcept;
  vector &operator=(vector &&) noexcept;
  T *data() const noexcept;
};
}
struct Record {
  Record() noexcept;
  Record(const Record &) noexcept;
  Record &operator=(const Record &) noexcept;
};
void consume(Record &&) noexcept;
void copies() {
  Record source, target;
  consume(std::move(source)); // expected-note 2 {{moved here}}
  Record copied(source); // expected-warning {{use of moved-from value 'source'}}
  target = source; // expected-warning {{use of moved-from value 'source'}}
}

int unique_borrow(bool self) {
  std::unique_ptr<int> pointer(new int(1)), other(new int(2));
  int *borrowed = pointer.get();
  pointer = std::move(self ? pointer : other); // expected-note {{invalidated by non-const use of owner}}
  return *borrowed; // expected-warning {{use of invalid pointer 'borrowed'}}
}

int vector_borrow(bool self) {
  std::vector<int> values(1), other(2);
  int *borrowed = values.data();
  values = std::move(self ? values : other); // expected-note {{invalidated by non-const use of owner}}
  return *borrowed; // expected-warning {{use of invalid pointer 'borrowed'}}
}

int unique_self() {
  std::unique_ptr<int> pointer(new int(1));
  auto &alias = pointer;
  pointer = std::move(alias);
  return *pointer;
}

int uncertain_receiver(bool condition) {
  std::unique_ptr<int> source(new int(1)), target(new int(2));
  int *borrowed = source.get();
  int *oldTarget = target.get();
  (condition ? source : target) = std::move(source); // expected-note {{invalidated by non-const use of owner}}
  return *borrowed + *oldTarget; // expected-warning {{use of invalid pointer 'oldTarget'}}
}

int scope_exit(bool condition) {
  int *pointer;
  {
    int first = 1, second = 2;
    pointer = condition ? &first : &second;
  } // expected-note {{pointee 'second' invalidated at end of scope}}
  return *pointer; // expected-warning {{use of invalid pointer 'pointer'}}
}

struct Plain { int value; };
void consume(Plain &&) noexcept;
void observe(const Plain &) noexcept;
void trivial_copies() {
  Plain source{1}, target{};
  consume(std::move(source)); // expected-note 2 {{moved here}}
  target = source; // expected-warning {{use of moved-from value 'source'}}
  Plain copied(source); // expected-warning {{use of moved-from value 'source'}}
  observe(target);
  observe(copied);
}

void observe(const Record &) noexcept;
void target_order(bool choose) {
  Record first, second;
  consume(std::move(choose ? first : second)); // expected-note 2 {{moved here}}
  observe(choose ? first : second); // expected-warning {{use of moved-from value 'first'}} expected-warning {{use of moved-from value 'second'}}
}

void five_targets(unsigned index) {
  Record first, second, third, fourth, fifth;
  consume(std::move(first)); // expected-note {{moved here}}
  consume(std::move(second)); // expected-note {{moved here}}
  consume(std::move(third)); // expected-note {{moved here}}
  consume(std::move(fourth)); // expected-note {{moved here}}
  consume(std::move(fifth)); // expected-note {{moved here}}
  Record *selected = index == 0 ? &first : index == 1 ? &second :
                     index == 2 ? &third : index == 3 ? &fourth : &fifth;
  observe(*selected); // expected-warning {{use of moved-from value 'first'}} expected-warning {{use of moved-from value 'second'}} expected-warning {{use of moved-from value 'third'}} expected-warning {{use of moved-from value 'fourth'}} expected-warning {{use of moved-from value 'fifth'}}
}

struct [[gsl::Owner(int)]] CustomOwner {
  CustomOwner() noexcept;
  CustomOwner &operator=(const CustomOwner &) noexcept;
};
void consume(CustomOwner &&) noexcept;
void uncertain_owner_copy(bool condition) {
  CustomOwner source, first, second;
  consume(std::move(source)); // expected-note {{moved here}}
  (condition ? first : second) = source; // expected-warning {{use of invalid pointer 'source'}}
}

struct MemberReceiver {
  std::unique_ptr<int> member;
  int transfer(int *raw);
};
int MemberReceiver::transfer(int *raw) {
  std::unique_ptr<int> source(raw);
  member = std::move(source); // expected-note {{moved here}}
  return *source; // expected-warning {{use of invalid pointer 'source'}}
}

int unknown_receiver(unsigned long address, int *raw) {
  auto *destination = reinterpret_cast<std::unique_ptr<int> *>(address);
  std::unique_ptr<int> source(raw);
  *destination = std::move(source); // expected-note {{moved here}}
  return *source; // expected-warning {{use of invalid pointer 'source'}}
}

// CHECK: warning: use of moved-from value 'source'
// CHECK: warning: use of moved-from value 'source'
// CHECK: warning: use of invalid pointer 'borrowed'
// CHECK: warning: use of invalid pointer 'borrowed'
// CHECK: warning: use of invalid pointer 'oldTarget'
// CHECK: warning: use of invalid pointer 'pointer'
// CHECK: note: pointee 'second' invalidated at end of scope
// CHECK: warning: use of moved-from value 'source'
// CHECK: warning: use of moved-from value 'source'
// CHECK: warning: use of moved-from value 'first'
// CHECK: warning: use of moved-from value 'second'
// CHECK: warning: use of moved-from value 'first'
// CHECK: warning: use of moved-from value 'second'
// CHECK: warning: use of moved-from value 'third'
// CHECK: warning: use of moved-from value 'fourth'
// CHECK: warning: use of moved-from value 'fifth'
// CHECK: warning: use of invalid pointer 'source'
// CHECK: warning: use of invalid pointer 'source'
// CHECK: warning: use of invalid pointer 'source'
