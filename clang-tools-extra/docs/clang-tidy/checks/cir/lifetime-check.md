```{title} clang-tidy - cir-lifetime-check
```

# cir-lifetime-check

Detects uses of invalid pointers with ClangIR (CIR) lifetime analysis, including
null and uninitialized pointers, pointers to locals whose scope has ended, and
selected invalidations involving owner and pointer types.
It also diagnoses reviewed use-after-move cases involving tracked local values,
rvalue-reference parameters, and move construction or assignment of supported
owner and pointer types.

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

```cpp
void consume(int &&);
int moved() {
  int value = 42;
  consume(static_cast<int &&>(value)); // note: moved here via std::move or rvalue reference
  return value; // warning: use of moved-from value 'value'
}
```

Ordinary scalar copies do not mark their source as moved. For tracked scalar
values, the check preserves an explicit move-intent policy: initialization or
assignment from `std::move(value)` or an equivalent rvalue cast consumes the
source for this analysis, even though the C++ scalar operation itself is a copy.
Merely forming the cast or binding a reference does not consume the value.
When lifetime analysis is enabled, CIR records an xvalue read with the serialized
`source_is_xvalue` load attribute, printed as `cir.load xvalue`. This distinction
also survives writing and reloading CIR.

Recognized `std::unique_ptr` and `std::shared_ptr` owners permit safe operations
such as `get`, `release`, `reset`, and conversion to `bool` after a move.
Dereferencing the moved-from pointer is diagnosed. Parameter reference
categories and method identity come from serialized CIR metadata; argument move
tracking is skipped when CIRGen cannot prove the source-parameter mapping.

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

The analysis uses an approximation for loops and joins. Structured branches
proven to return do not contribute pointer state to subsequent code. General
control-flow, interprocedural, exception, and conditional-expression analysis
remain limited. Passing an aggregate with a null pointer field to an implicit
assignment can produce a conservative false positive. Valid reinitialization,
including scalar reassignment and an owner's `reset(new T)`, is not fully
handled and remains follow-up work. Structured bindings and other compound
operations can also produce conservative false positives; this check does not
provide general member-sensitive move tracking.

Capture handling is partial. Some direct local-owner lambda init-captures are
handled through move construction; remaining capture semantics need follow-up.
General lambda escape and moved-callback analysis are not supported. Diagnostic
counts alone are not a measure of accuracy.
