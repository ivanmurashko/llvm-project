// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -std=c++20 -fclangir -fclangir-lifetime-check="history=all" -verify -emit-cir %s -o /dev/null
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -std=c++20 -fclangir -fclangir-lifetime-check -clangir-disable-passes -emit-cir %s -o %t.cir
// RUN: cir-opt %t.cir --cir-canonicalize --cir-lifetime-check="history=all" -o /dev/null 2>&1 | FileCheck %s

namespace std {
template <class T> class [[gsl::Owner(T)]] unique_ptr {
public:
  void reset(T *) noexcept;
  explicit operator bool() const noexcept;
  T &operator*() const noexcept;
};
} // namespace std

int conditional_replacement_keeps_unknown_input(std::unique_ptr<int> &owner,
                                                bool condition) {
  if (condition)
    owner.reset(new int(1));
  if (!owner)
    return *owner; // expected-warning {{use of invalid pointer 'owner'}}
  return 1;
}

int both_paths_replace_unknown_input(std::unique_ptr<int> &owner,
                                    bool condition) {
  if (condition)
    owner.reset(new int(1));
  else
    owner.reset(new int(2));
  if (!owner)
    return *owner;
  return 1;
}

// CHECK: warning: use of invalid pointer 'owner'
// CHECK-NOT: warning:
