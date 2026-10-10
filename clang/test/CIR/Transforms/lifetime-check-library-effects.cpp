// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -std=c++20 -fclangir -fclangir-lifetime-check="history=all" -verify -emit-cir %s -o %t.cir
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -std=c++20 -fclangir -fclangir-lifetime-check -clangir-disable-passes -emit-cir %s -o %t.raw.cir
// RUN: cir-opt %t.raw.cir --cir-canonicalize --cir-lifetime-check="history=all" -o /dev/null 2>&1 | FileCheck %s --allow-empty --check-prefix=CLEAN
// RUN: cir-opt %t.raw.cir --emit-bytecode -o %t.bc
// RUN: cir-opt %t.bc --cir-lifetime-check="history=all" -o /dev/null 2>&1 | FileCheck %s --allow-empty --check-prefix=CLEAN
// expected-no-diagnostics
// CLEAN-NOT: warning:
// CLEAN-NOT: error:

// These declarations describe the standard-library contracts. No analysis of
// library implementation details is needed to know the effects of reset.
namespace std {
template <class T> T &&move(T &value) { return static_cast<T &&>(value); }
template <class T> class [[gsl::Owner(T)]] unique_ptr {
public:
  unique_ptr() noexcept;
  explicit unique_ptr(T *) noexcept;
  unique_ptr(unique_ptr &&) noexcept;
  unique_ptr &operator=(unique_ptr &&) noexcept;
  void reset(T * = nullptr) noexcept;
  T *get() const noexcept;
  T *release() noexcept;
  explicit operator bool() const noexcept;
  T &operator*() const noexcept;
};
template <class T> class [[gsl::Owner(T)]] shared_ptr {
public:
  explicit shared_ptr(T *);
  shared_ptr(shared_ptr &&) noexcept;
  shared_ptr &operator=(const shared_ptr &) noexcept;
  void reset(T *);
  T *get() const noexcept;
  explicit operator bool() const noexcept;
  T &operator*() const noexcept;
};
} // namespace std

int *unknown_pointer() noexcept;

int reset_nonnull() {
  std::unique_ptr<int> source(new int(1));
  std::unique_ptr<int> destination(std::move(source));
  source.reset(new int(2));
  return *source;
}

void reset_unknown() {
  std::unique_ptr<int> source(new int(1));
  std::unique_ptr<int> destination(std::move(source));
  source.reset(unknown_pointer());
  // Unknown contents do not prove nullness and must not retain the old move.
  (void)*source;
}

int move_assignment_reinitializes() {
  std::unique_ptr<int> source(new int(1));
  std::unique_ptr<int> destination(std::move(source));
  std::unique_ptr<int> replacement(new int(2));
  source = std::move(replacement);
  return *source;
}

int copy_assignment_reinitializes() {
  std::shared_ptr<int> source(new int(1));
  std::shared_ptr<int> destination(std::move(source));
  std::shared_ptr<int> replacement(new int(2));
  source = replacement;
  return *source;
}

void reset_keeps_safe_observers() {
  std::unique_ptr<int> source(new int(1));
  std::unique_ptr<int> destination(std::move(source));
  source.reset();
  (void)source.get();
  (void)static_cast<bool>(source);
  (void)source.release();
}
