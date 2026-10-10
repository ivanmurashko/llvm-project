// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -std=c++17 -fclangir -fclangir-lifetime-check="history=all" -verify=disabled -emit-cir %s -o /dev/null
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -std=c++17 -fexceptions -fcxx-exceptions -DEH_ENABLED -fclangir -fclangir-lifetime-check="history=all" -verify=eh -emit-cir %s -o /dev/null
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -std=c++17 -fclangir -clangir-disable-passes -emit-cir %s -o - | FileCheck %s --check-prefix=NO-EH

// cc1 defaults to exceptions disabled. Potentially throwing declarations do
// not require exceptional analysis states in this emitted CIR module.
// NO-EH: cir.lowering_lang_options = #cir.lowering_lang_options<exceptions = false
struct Item {
  explicit Item(int);
  ~Item() noexcept;
  int value;
};
#define PARAMS_2 const Item &, const Item &
#define PARAMS_8 PARAMS_2, PARAMS_2, PARAMS_2, PARAMS_2
#define PARAMS_64 PARAMS_8, PARAMS_8, PARAMS_8, PARAMS_8, PARAMS_8, PARAMS_8, PARAMS_8, PARAMS_8
#define ITEMS_2 Item(1), Item(2)
#define ITEMS_8 ITEMS_2, ITEMS_2, ITEMS_2, ITEMS_2
#define ITEMS_64 ITEMS_8, ITEMS_8, ITEMS_8, ITEMS_8, ITEMS_8, ITEMS_8, ITEMS_8, ITEMS_8
void sink(PARAMS_64) noexcept;
void many_temporary_cleanups() { sink(ITEMS_64); }
// NO-EH-LABEL: cir.func {{.*}} @_Z23many_temporary_cleanupsv
// NO-EH: cir.call @_ZN4ItemC1Ei
// NO-EH: cir.cleanup.scope
// NO-EH: cleanup normal

[[noreturn]] void stop(int);
void noreturn_argument_diagnostic() {
  int *pointer = nullptr; // disabled-note {{'nullptr' invalidated here}} eh-note {{'nullptr' invalidated here}}
  stop(*pointer); // disabled-warning {{use of invalid pointer 'pointer'}} eh-warning {{use of invalid pointer 'pointer'}}
}

#ifdef EH_ENABLED
void may_throw();
void observe(int) noexcept;
void consume(int &&) noexcept;
struct Guard { ~Guard() noexcept; };

void null_before_possible_throw() {
  int live = 1;
  int *pointer = &live;
  try {
    Guard guard;
    pointer = nullptr; // eh-note {{'nullptr' invalidated here}}
    may_throw();
    pointer = &live;
  } catch (...) {
    observe(*pointer); // eh-warning {{use of invalid pointer 'pointer'}}
  }
}

// noexcept describes escape from this function. It must not suppress a
// potentially throwing call's path to a catch inside the function.
void internal_catch_in_noexcept() noexcept {
  int live = 1;
  int *pointer = &live;
  try {
    Guard guard;
    pointer = nullptr; // eh-note {{'nullptr' invalidated here}}
    may_throw();
    pointer = &live;
  } catch (...) {
    observe(*pointer); // eh-warning {{use of invalid pointer 'pointer'}}
  }
}

void moved_before_possible_throw() {
  int value = 1;
  try {
    consume(static_cast<int &&>(value)); // eh-note {{moved here via std::move or rvalue reference}}
    may_throw();
    value = 2;
  } catch (...) {
    observe(value); // eh-warning {{use of moved-from value 'value'}}
  }
}

void live_borrow_in_catch() {
  int live = 1;
  int *pointer = &live;
  try { may_throw(); }
  catch (...) { observe(*pointer); }
}
#endif
