// RUN: %clang_cc1 -std=c++20 -triple x86_64-unknown-linux-gnu -I%S/Inputs -mconstructor-aliases -fclangir -fclangir-lifetime-check="history=all" -verify -emit-cir %s -o %t.cir


#include "std-cxx.h"

// Known conservative false positive: aggregate assignment checks the null
// pointer field of the temporary as if the callee dereferenced that field.

typedef enum SType {
  INFO_ENUM_0 = 9,
  INFO_ENUM_1 = 2020,
} SType;

typedef struct InfoRaw { // expected-note 2 {{invalidated here}}
    SType type;
    const void* __attribute__((__may_alias__)) next;
    unsigned u;
} InfoRaw;

void swappy(unsigned c) {
  std::vector<InfoRaw> images(c);
  for (auto& image : images) {
    image = {INFO_ENUM_1}; // expected-warning {{passing aggregate containing invalid pointer member}}
  }

  std::vector<InfoRaw> images2(c);
  for (unsigned i = 0; i < c; i++) {
    images2[i] = {INFO_ENUM_1}; // expected-warning {{passing aggregate containing invalid pointer member}}
  }
}
