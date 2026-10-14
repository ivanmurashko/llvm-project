// RUN: clang-tidy %s --checks='-*,cir-lifetime-check' --config='{}' -- -target x86_64-unknown-linux-gnu -std=c11 2>&1 | count 0
// RUN: clang-tidy %s --checks='-*,cir-lifetime-check' --config='{CheckOptions: {cir-lifetime-check.AnalyzeUnusedMainFileFunctions: true}}' -- -target x86_64-unknown-linux-gnu -std=c11 2>&1 | FileCheck %s --implicit-check-not='warning:' --implicit-check-not='error:' --implicit-check-not='Assertion'

void alias_target(void) {}

// An alias is a definition for redeclaration lookup, but has no analysis body.
inline void inline_alias(void)
    __attribute__((gnu_inline, alias("alias_target")));
inline void declaration_only(void);

// Prove that opt-in selection is active while the bodyless declarations above
// remain outside the root list.
static inline void actual_body(void) {
  int *body_pointer = 0;
  *body_pointer = 1;
}
// CHECK: warning: use of invalid pointer 'body_pointer' [cir-lifetime-check]
