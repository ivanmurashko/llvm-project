// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -std=c++20 -O0 -fclangir -emit-cir %s -o /dev/null -verify
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -std=c++20 -O3 -fclangir -emit-cir %s -o /dev/null -verify
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -std=c++20 -O0 -fclangir -fclangir-lifetime-check -emit-cir %s -o /dev/null -verify
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -std=c++20 -O3 -fclangir -fclangir-lifetime-check -emit-cir %s -o /dev/null -verify
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -std=c++20 -DNESTED_LABEL -fclangir -emit-cir %s -o /dev/null -verify
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -std=c++20 -DNESTED_ATTRIBUTE -fclangir -emit-cir %s -o /dev/null -verify

struct Resource {
  Resource();
  ~Resource();
  explicit operator bool() const;
};

// A handled statement can fail while emitting a child. Propagate that failure
// instead of treating the statement as unhandled and trying to emit it again.
void nested(bool condition) {
  if (condition) {
#if defined(NESTED_LABEL)
  label:
#elif defined(NESTED_ATTRIBUTE)
    [[likely]]
#endif
    while (const Resource &value = Resource()) {} // expected-error {{ClangIR code gen Not Yet Implemented: loop condition with a lifetime-extended temporary requiring destruction}}
  }
}
