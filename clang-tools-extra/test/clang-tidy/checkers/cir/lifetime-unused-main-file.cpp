// RUN: clang-tidy %s --checks='-*,cir-lifetime-check' --config='{}' -- -target x86_64-unknown-linux-gnu -std=c++20 2>&1 | count 0
// RUN: clang-tidy %s --checks='-*,cir-lifetime-check' --config='{CheckOptions: {cir-lifetime-check.AnalyzeUnusedMainFileFunctions: false}}' -- -target x86_64-unknown-linux-gnu -std=c++20 2>&1 | count 0
// RUN: clang-tidy %s --checks='-*,cir-lifetime-check' --config='{HeaderFilterRegex: ".*", CheckOptions: {cir-lifetime-check.AnalyzeUnusedMainFileFunctions: true}}' --export-fixes=%t.yaml -- -target x86_64-unknown-linux-gnu -std=c++20 > %t.messages 2>&1
// RUN: FileCheck %s --check-prefix=ENABLED --implicit-check-not='warning:' --implicit-check-not='error:' --input-file=%t.messages
// RUN: FileCheck %s --check-prefix=COUNT --input-file=%t.messages
// RUN: FileCheck %s --input-file=%t.yaml --check-prefix=YAML
// RUN: clang-tidy %s --checks='-*,cir-lifetime-check' --config='{}' --dump-config -- | FileCheck %s --check-prefix=DEFAULT-OPTION
// RUN: clang-tidy %s --checks='-*,cir-lifetime-check' --config='{CheckOptions: {cir-lifetime-check.AnalyzeUnusedMainFileFunctions: true}}' --dump-config -- | FileCheck %s --check-prefix=ENABLED-OPTION

// COUNT-COUNT-10: warning:
// COUNT-NOT: warning:

// DEFAULT-OPTION: cir-lifetime-check.AnalyzeUnusedMainFileFunctions: 'false'
// ENABLED-OPTION: cir-lifetime-check.AnalyzeUnusedMainFileFunctions: 'true'

// Definition expansion location controls eligibility, not a redeclaration's
// location or the macro spelling location. Header-only bodies remain deferred.
inline void main_declaration_header_definition();
#define MAIN_MACRO(Name) inline void Name(unsigned value) { (void)__builtin_elementwise_clzg(value); }
#include "Inputs/lifetime-unused-main-file.h"
#undef MAIN_MACRO

inline void unused_free() {
  int *free_pointer = nullptr;
  *free_pointer = 1;
}
// ENABLED-DAG: warning: use of invalid pointer 'free_pointer' [cir-lifetime-check]

inline void duplicate();
inline void duplicate();
inline void duplicate() {
  int *duplicate_pointer = nullptr;
  *duplicate_pointer = 1;
}
// ENABLED-DAG: warning: use of invalid pointer 'duplicate_pointer' [cir-lifetime-check]

inline void header_declaration_main_definition() {
  int *definition_pointer = nullptr;
  *definition_pointer = 1;
}
// ENABLED-DAG: warning: use of invalid pointer 'definition_pointer' [cir-lifetime-check]

HEADER_MACRO(header_macro_in_main, macro_pointer)
// ENABLED-DAG: warning: use of invalid pointer 'macro_pointer' [cir-lifetime-check]

namespace wrapped {
inline void in_namespace() {
  int *namespace_pointer = nullptr;
  *namespace_pointer = 1;
}
}
// ENABLED-DAG: warning: use of invalid pointer 'namespace_pointer' [cir-lifetime-check]

extern "C" {
inline void in_linkage() {
  int *linkage_pointer = nullptr;
  *linkage_pointer = 1;
}
}
// ENABLED-DAG: warning: use of invalid pointer 'linkage_pointer' [cir-lifetime-check]

struct Methods {
  void ordinary() {
    int *method_pointer = nullptr;
    *method_pointer = 1;
  }
  static void static_method() {
    int *static_pointer = nullptr;
    *static_pointer = 1;
  }
  friend void friend_definition(Methods &) {
    int *friend_pointer = nullptr;
    *friend_pointer = 1;
  }
};
// ENABLED-DAG: warning: use of invalid pointer 'method_pointer' [cir-lifetime-check]
// ENABLED-DAG: warning: use of invalid pointer 'static_pointer' [cir-lifetime-check]
// ENABLED-DAG: warning: use of invalid pointer 'friend_pointer' [cir-lifetime-check]

// Selected roots still use normal dependency emission, including headers.
inline void root_with_header_dependency() { required_header_dependency(); }
// ENABLED-DAG: warning: use of invalid pointer 'dependency_pointer' [cir-lifetime-check]
// YAML: DiagnosticName:  cir-lifetime-check
// YAML: Level:           Warning

// These bodies are intentionally unsupported by CIRGen. Selecting one would
// turn this otherwise valid opt-in run into a generation error.
template <class T> inline void uninstantiated(unsigned value) {
  (void)__builtin_elementwise_clzg(value);
}
template <> inline void uninstantiated<int>(unsigned value) {
  (void)__builtin_elementwise_clzg(value);
}
template <class T> struct TemplateRecord {
  void method(unsigned value) { (void)__builtin_elementwise_clzg(value); }
};
template <> struct TemplateRecord<int> {
  void method(unsigned value) { (void)__builtin_elementwise_clzg(value); }
};
struct Excluded {
  Excluded() { unsigned value = 1; (void)__builtin_elementwise_clzg(value); }
  ~Excluded() { unsigned value = 1; (void)__builtin_elementwise_clzg(value); }
  virtual void virtual_method(unsigned value) { (void)__builtin_elementwise_clzg(value); }
};
struct DefaultedDeleted {
  DefaultedDeleted() = default;
  DefaultedDeleted(const DefaultedDeleted &) = delete;
};
consteval unsigned immediate(unsigned value) {
  return __builtin_elementwise_clzg(value);
}

// Do not descend into a function body to collect local classes or lambdas.
// The discarded branch has no normal dependency on either body.
inline void root_with_discarded_local_definitions() {
  if constexpr (false) {
    struct Local {
      void method(unsigned value) { (void)__builtin_elementwise_clzg(value); }
    };
    auto lambda = [](unsigned value) { return __builtin_elementwise_clzg(value); };
  }
}
