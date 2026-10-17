// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -std=c++20 -fexceptions -fcxx-exceptions -fclangir -fclangir-lifetime-check="history=all" -verify -emit-cir %s -o /dev/null
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -std=c++20 -fexceptions -fcxx-exceptions -fclangir -fclangir-lifetime-check -clangir-disable-passes -emit-cir %s -o %t.cir
// RUN: cir-opt %t.cir --mlir-disable-threading --cir-lifetime-check="history=all" -o /dev/null 2> %t.serial
// RUN: FileCheck %s --input-file=%t.serial --implicit-check-not='warning:' --implicit-check-not='error:'
// RUN: cir-opt %t.cir --mlir-disable-threading=false --cir-lifetime-check="history=all" -o /dev/null 2> %t.parallel
// RUN: diff %t.serial %t.parallel
// RUN: cir-opt %t.cir --cir-canonicalize --mlir-print-debuginfo -o %t.canonical.cir
// RUN: cir-opt %t.canonical.cir --cir-lifetime-check="history=all" -o /dev/null 2> %t.text
// RUN: diff %t.serial %t.text
// RUN: cir-opt %t.canonical.cir --emit-bytecode -o %t.bc
// RUN: cir-opt %t.bc --cir-lifetime-check="history=all" -o /dev/null 2> %t.bytecode
// RUN: diff %t.serial %t.bytecode

namespace std {
template <class T> T &&move(T &value) { return static_cast<T &&>(value); }
template <class T> class [[gsl::Owner(T)]] unique_ptr {
public:
  unique_ptr() noexcept;
  explicit unique_ptr(T *) noexcept;
  unique_ptr(unique_ptr &&) noexcept;
  unique_ptr &operator=(unique_ptr &&) noexcept;
  explicit operator bool() const noexcept;
  T &operator*() const noexcept;
};
template <class T> class shared_ptr {
public:
  explicit shared_ptr(T *) noexcept;
  shared_ptr(shared_ptr &&) noexcept;
  shared_ptr &operator=(shared_ptr &&) noexcept;
  T *get() const noexcept;
  T &operator*() const noexcept;
};
template <class T> class optional {
public:
  optional(T) noexcept;
  optional(optional &&) noexcept;
  bool has_value() const noexcept;
  T &operator*() const noexcept;
};
template <class T> class weak_ptr {
public:
  bool expired() const noexcept;
};
} // namespace std

void consume(std::unique_ptr<int> &&) noexcept;
void consume(std::shared_ptr<int> &&) noexcept;
void consume(std::weak_ptr<int> &&) noexcept;

// A smart pointer observed nonempty cannot be the empty moved-from alternative
// from a previous iteration. The test intentionally uses an opaque consumer.
void guarded_reuse(bool again) {
  std::unique_ptr<int> source;
  while (again) {
    if (!source)
      source = std::unique_ptr<int>(new int(1));
    (void)*source;
    if (source)
      consume(std::move(source));
  }
  if (source)
    consume(std::move(source));
}

// Refining the nonempty branch must not discard the empty continuation.
int use_after_empty_if() {
  std::unique_ptr<int> source(new int(1));
  consume(std::move(source)); // expected-note {{moved here}}
  if (source) {}
  return *source; // expected-warning {{use of invalid pointer 'source'}}
}

int move_inside_guard(std::unique_ptr<int> &source) {
  if (source) {
    consume(std::move(source)); // expected-note {{moved here}}
    return *source; // expected-warning {{use of invalid pointer 'source'}}
  }
  return 0;
}

int stale_guard() {
  std::unique_ptr<int> source(new int(1));
  bool present = static_cast<bool>(source);
  consume(std::move(source)); // expected-note {{moved here}}
  if (present)
    return *source; // expected-warning {{use of invalid pointer 'source'}}
  return 0;
}

int optional_presence_does_not_restore() {
  std::optional<int> source(1);
  std::optional<int> destination(std::move(source)); // expected-note {{moved here}}
  if (source.has_value())
    return *source; // expected-warning {{use of invalid pointer 'source'}}
  return 0;
}

int shared_self_move() {
  std::shared_ptr<int> pointer(new int(1));
  auto &alias = pointer;
  pointer = std::move(alias);
  return *pointer;
}

int shared_possible_self_move(bool self) {
  std::shared_ptr<int> pointer(new int(1)), other(new int(2));
  pointer = std::move(self ? pointer : other);
  return *pointer;
}

int shared_empty_self_move() {
  std::shared_ptr<int> pointer(nullptr); // expected-note {{'nullptr' invalidated here}}
  auto &alias = pointer;
  pointer = std::move(alias);
  return *pointer; // expected-warning {{use of moved-from value 'pointer'}}
}

int shared_other_can_be_consumed(bool self) {
  std::shared_ptr<int> pointer(new int(1)), other(new int(2));
  pointer = std::move(self ? pointer : other); // expected-note {{moved here}}
  return *other; // expected-warning {{use of moved-from value 'other'}}
}

int shared_previous_borrow_can_expire(bool self) {
  std::shared_ptr<int> pointer(new int(1)), other(new int(2));
  int *borrowed = pointer.get();
  pointer = std::move(self ? pointer : other); // expected-note {{invalidated by non-const use of owner}}
  return *borrowed; // expected-warning {{use of invalid pointer 'borrowed'}}
}

struct Buffer {
  Buffer() noexcept;
  ~Buffer() noexcept;
  Buffer &operator=(int) noexcept;
  Buffer &operator=(double);
  Buffer &operator+=(int) noexcept;
  const Buffer &operator=(unsigned) const noexcept;
  int read() const noexcept;
};
void consume(Buffer &&) noexcept;
void destroy(Buffer *) noexcept;
void inspect(const Buffer *) noexcept;
void observe(const Buffer &) noexcept;

int converting_assignment_restores() {
  Buffer source;
  consume(std::move(source));
  source = 1;
  return source.read();
}

void assignment_uses_rhs_first() {
  Buffer source;
  consume(std::move(source)); // expected-note {{moved here}}
  source = source.read(); // expected-warning {{use of moved-from value 'source'}}
  (void)source.read();
}

void compound_assignment_does_not_restore() {
  Buffer source;
  consume(std::move(source)); // expected-note 2 {{moved here}}
  source += 1; // expected-warning {{use of moved-from value 'source'}}
  (void)source.read(); // expected-warning {{use of moved-from value 'source'}}
}

void const_assignment_does_not_restore() {
  Buffer source;
  consume(std::move(source)); // expected-note 2 {{moved here}}
  static_cast<const Buffer &>(source) = 1U; // expected-warning {{use of moved-from value 'source'}}
  (void)source.read(); // expected-warning {{use of moved-from value 'source'}}
}

void throwing_assignment_restores_only_on_success() {
  Buffer source;
  consume(std::move(source)); // expected-note {{moved here}}
  try {
    source = 1.0;
    (void)source.read();
  } catch (...) {
    (void)source.read(); // expected-warning {{use of moved-from value 'source'}}
  }
}

// Moving the record does not move the pointer, and delete is valid for the
// moved-from object. A reference observer still uses the moved-from record.
void pointer_value_does_not_read_pointee(Buffer *pointer) {
  consume(std::move(*pointer));
  inspect(pointer);
  destroy(pointer);
}

void delete_does_not_read_moved_pointee(Buffer *pointer) {
  consume(std::move(*pointer));
  delete pointer;
}

void reference_still_reads_pointee(Buffer *pointer) {
  consume(std::move(*pointer)); // expected-note {{moved here}}
  observe(*pointer); // expected-warning {{use of moved-from value 'fn_arg:0'}}
}

struct [[gsl::Pointer(int)]] View {
  View(int *) noexcept;
  View &operator=(int) noexcept;
  int read() const noexcept;
};
void consume(View &&) noexcept;

void pointer_assignment_keeps_points_to_policy() {
  int value = 1;
  View source(&value);
  consume(std::move(source)); // expected-note 2 {{moved here}}
  source = 1; // expected-warning {{use of invalid pointer 'source'}}
  (void)source.read(); // expected-warning {{use of invalid pointer 'source'}}
}

struct Payload { int first, second; };
void consume(Payload &&) noexcept;
void take(Payload) noexcept;

// A by-value record is read by its argument materialization. The pointer
// passing rule must not suppress that earlier copy/load diagnostic.
void materialized_record_argument() {
  Payload value{1, 2};
  consume(std::move(value)); // expected-note {{moved here}}
  take(value); // expected-warning {{use of moved-from value 'value'}}
}

void uncertain_assignment_receiver(bool choose) {
  Buffer first, second;
  consume(std::move(first)); // expected-note {{moved here}}
  (choose ? first : second) = 1;
  (void)first.read(); // expected-warning {{use of moved-from value 'first'}}
}

void guarded_weak_reuse(std::weak_ptr<int> &source) {
  consume(std::move(source));
  if (!source.expired())
    consume(std::move(source));
}

void weak_empty_join_is_kept(std::weak_ptr<int> &source) {
  consume(std::move(source)); // expected-note {{moved here}}
  if (!source.expired()) {}
  consume(std::move(source)); // expected-warning {{'source'}}
}

// CHECK: warning: use of invalid pointer 'source'
// CHECK: warning: use of invalid pointer 'source'
// CHECK: warning: use of invalid pointer 'source'
// CHECK: warning: use of invalid pointer 'source'
// CHECK: warning: use of moved-from value 'pointer'
// CHECK: warning: use of moved-from value 'other'
// CHECK: warning: use of invalid pointer 'borrowed'
// CHECK: warning: use of moved-from value 'source'
// CHECK: warning: use of moved-from value 'source'
// CHECK: warning: use of moved-from value 'source'
// CHECK: warning: use of moved-from value 'source'
// CHECK: warning: use of moved-from value 'source'
// CHECK: warning: use of moved-from value 'source'
// CHECK: warning: use of moved-from value 'fn_arg:0'
// CHECK: warning: use of invalid pointer 'source'
// CHECK: warning: use of invalid pointer 'source'
// CHECK: warning: use of moved-from value 'value'
// CHECK: warning: use of moved-from value 'first'
// CHECK: warning: {{.*}}'source'
