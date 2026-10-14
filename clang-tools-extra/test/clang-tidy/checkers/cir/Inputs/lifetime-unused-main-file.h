#ifndef LIFETIME_UNUSED_MAIN_FILE_H
#define LIFETIME_UNUSED_MAIN_FILE_H

inline void header_declaration_main_definition();
inline void main_declaration_header_definition() {
  unsigned value = 1;
  (void)__builtin_elementwise_clzg(value);
}
inline void unused_header_definition(unsigned value) {
  (void)__builtin_elementwise_clzg(value);
}

#define HEADER_MACRO(Name, Pointer) \
  inline void Name() { int *Pointer = nullptr; *Pointer = 1; }

MAIN_MACRO(main_macro_in_header)

inline void required_header_dependency() {
  int *dependency_pointer = nullptr;
  *dependency_pointer = 1;
}

#endif
