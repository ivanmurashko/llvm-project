// RUN: %clang_cc1 -std=c++17 -triple x86_64-unknown-linux-gnu -fclangir -fclangir-lifetime-check="history=all" -verify -emit-cir %s -o /dev/null
// RUN: %clang_cc1 -std=c++17 -triple x86_64-unknown-linux-gnu -O3 -fclangir -fclangir-lifetime-check="history=all" -verify -emit-cir %s -o /dev/null
// RUN: %clang_cc1 -std=c++17 -triple x86_64-unknown-linux-gnu -fclangir -fclangir-lifetime-check -clangir-disable-passes -emit-cir %s -o %t.cir
// RUN: cir-opt %t.cir --cir-canonicalize --cir-lifetime-check="history=all" --verify-diagnostics -o /dev/null
// RUN: cir-opt %t.cir --cir-canonicalize --cir-lifetime-check="history=all" --verify-diagnostics --emit-bytecode -o %t.bc
// RUN: cir-opt %t.bc --cir-lifetime-check="history=all" --verify-diagnostics -o /dev/null
// RUN: %clang_cc1 -std=c++17 -triple x86_64-unknown-linux-gnu -O3 -fclangir -fclangir-lifetime-check -clangir-disable-passes -emit-cir %s -o %t.o3.cir
// RUN: cir-opt %t.o3.cir --cir-canonicalize --cir-lifetime-check="history=all" --verify-diagnostics -o /dev/null
// RUN: cir-opt %t.o3.cir --cir-canonicalize --cir-lifetime-check="history=all" --verify-diagnostics --emit-bytecode -o %t.o3.bc
// RUN: cir-opt %t.o3.bc --cir-lifetime-check="history=all" --verify-diagnostics -o /dev/null

void observe(int);

// The previous iteration's local is dead even before the next iteration
// reaches its declaration. The initial pointer is valid, so the diagnostic
// requires the loop backedge and the per-iteration lifetime end.
void previous_iteration(int (&input)[4]) {
  int initial = 0;
  int *escaped = &initial;
  for (int element : input) {
    observe(*escaped); // expected-warning {{use of invalid pointer 'escaped'}}
    int local = element;
    escaped = &local;
  } // expected-note {{pointee 'local' invalidated at end of scope}}
}

void continue_ends_iteration(int (&input)[4]) {
  int initial = 0;
  int *escaped = &initial;
  for (int element : input) {
    observe(*escaped); // expected-warning {{use of invalid pointer 'escaped'}}
    int local = element;
    escaped = &local;
    continue;
  } // expected-note {{pointee 'local' invalidated at end of scope}}
}

void loop_variable_ends_on_break(int (&input)[4]) {
  int initial = 0;
  int *escaped = &initial;
  for (int element : input) {
    escaped = &element;
    break;
  } // expected-note {{pointee 'element' invalidated at end of scope}}
  observe(*escaped); // expected-warning {{use of invalid pointer 'escaped'}}
}

void outer_storage_stays_live(int (&input)[4]) {
  int outer = 0;
  int *pointer = &outer;
  for (int element : input) {
    observe(*pointer);
    int *current = &element;
    observe(*current);
  }
  observe(*pointer);
}
