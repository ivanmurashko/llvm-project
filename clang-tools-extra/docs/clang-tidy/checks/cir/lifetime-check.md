```{title} clang-tidy - cir-lifetime-check
```

# cir-lifetime-check

Detects uses of invalid pointers with ClangIR (CIR) lifetime analysis, including
null and uninitialized pointers, pointers to locals whose scope has ended, and
selected invalidations involving owner and pointer types.

This experimental check is available only in builds configured with
`CLANG_ENABLE_CIR=ON`. It generates CIR from the translation unit's existing AST
and compilation options, verifies the module, and runs CIR canonicalization
followed by lifetime analysis.

```cpp
void example() {
  int *p;
  {
    int value = 0;
    p = &value;
  }
  *p = 42; // warning: use of invalid pointer 'p'
}
```

The warning includes a note identifying where `value` went out of scope. Warnings,
notes, and optional analysis remarks are also available through clang-tidy's
`--export-fixes` YAML output. This check does not provide fixes.

## Options

### HistoryList

A semicolon-separated list of diagnostic history categories: `invalid`, `null`,
or `all`. The default is `all`. An empty string disables history notes.

### RemarksList

A semicolon-separated list of analysis remarks: `pset-invalid` prints the pointer
set at invalid dereferences; `pset-always` prints it at every analyzed dereference;
`all` enables both. The default is an empty string, which disables remarks.

### HistLimit

The maximum number of invalidation history entries reported for a warning. The
default is `1`. Setting it to `0` disables invalidation history notes. Null-pointer
origin notes are controlled separately by `HistoryList`.

## Limitations

Coverage is limited to the functions and constructs supported and emitted by CIR
for the active compilation options. Code generation, verification, and pass
failures are errors; an unsuccessful run does not establish that the translation
unit is free of lifetime problems.

Switch analysis supports cases whose blocks execute in source order, including
labels connected by unconditional branches to the next block. Nested case
labels, other case control-flow graphs, and gotos in functions containing a
switch are reported as unsupported analysis errors.

The analysis uses an approximation for loops and joins and does not provide
general interprocedural analysis or full exception and conditional-expression
analysis. Passing an aggregate with a null pointer field to an implicit
assignment can produce a conservative false positive. Diagnostic counts alone
are not a measure of accuracy.
