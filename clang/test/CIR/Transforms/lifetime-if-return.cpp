// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -std=c++20 -fclangir -fclangir-lifetime-check="history=all" -emit-cir %s -o /dev/null -verify

// State on a path that returns must not reach the continuation of the if.
void then_returns(bool condition) {
  int value = 0;
  int *p = &value;
  if (condition) {
    p = nullptr;
    return;
  }
  *p = 1;
}

void else_returns(bool condition) {
  int value = 0;
  int *p = &value;
  if (condition) {
    *p = 1;
  } else {
    p = nullptr;
    return;
  }
  *p = 2;
}

void nested_returns(bool condition, bool inner) {
  int value = 0;
  int *p = &value;
  if (condition) {
    p = nullptr;
    if (inner) {
      return;
    } else {
      return;
    }
  }
  *p = 1;
}

struct Cleanup {
  ~Cleanup();
};

void cleanup_returns(bool condition) {
  int value = 0;
  int *p = &value;
  if (condition) {
    Cleanup cleanup;
    p = nullptr;
    return;
  }
  *p = 1;
}

// A continuing branch still contributes its state.
void then_continues(bool condition) {
  int value = 0;
  int *p = &value;
  if (condition) {
    p = nullptr; // expected-note {{invalidated here}}
  }
  *p = 1; // expected-warning {{use of invalid pointer 'p'}}
}

// A return in only one nested arm does not make the outer branch return.
void one_nested_return(bool condition, bool inner) {
  int value = 0;
  int *p = &value;
  if (condition) {
    if (inner)
      return;
    p = nullptr; // expected-note {{invalidated here}}
  }
  *p = 1; // expected-warning {{use of invalid pointer 'p'}}
}

// Keep diagnostics inside a returning branch.
void invalid_use_before_return(bool condition) {
  int value = 0;
  int *p = &value;
  if (condition) {
    p = nullptr; // expected-note {{invalidated here}}
    *p = 1; // expected-warning {{use of invalid pointer 'p'}}
    return;
  }
}

// Break and continue are loop paths, not function returns. Keep their state.
void break_path(bool condition) {
  int value = 0;
  int *p = &value;
  while (condition) {
    if (condition) {
      p = nullptr; // expected-note {{invalidated here}}
      break;
    }
  }
  *p = 1; // expected-warning {{use of invalid pointer 'p'}}
}

void continue_path(bool condition) {
  int value = 0;
  int *p = &value;
  while (condition) {
    if (condition) {
      p = nullptr; // expected-note {{invalidated here}}
      continue;
    }
  }
  *p = 1; // expected-warning {{use of invalid pointer 'p'}}
}

// A nested goto can skip the textual return. Do not discard its state.
void goto_path(bool condition, bool inner) {
  int value = 0;
  int *p = &value;
  if (condition) {
    p = nullptr; // expected-note {{invalidated here}}
    if (inner)
      goto out;
    return;
  }
out:
  *p = 1; // expected-warning {{use of invalid pointer 'p'}}
}
