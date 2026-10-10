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
followed by lifetime analysis. The analysis operates on CIR operations and
structured regions without constructing or analyzing an AST control-flow graph.

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

Verified `std::get` overloads for `std::pair` and `std::tuple` do not consume the
containing object when returning an element reference. Existing invalid-source
checks still apply; the contract does not establish general element aliasing or
memberwise move tracking. Unavailable reference contents in modeled library
calls remain unknown rather than becoming the reference's address.

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
Jumps into nested structured regions are also unsupported. Array constructor
and destructor regions are not generally modeled, including some forms that do
not cause an error.
A successful run does not establish coverage of every operation.

The analysis follows local aliases and captured value snapshots, joins
structured control-flow alternatives, and computes a finite loop fixed point.
Normal, return, break, continue, and exceptional continuations are separate.
Range-for loop-variable and body-local storage has a lifetime for each iteration,
with cleanup before the increment and on exit. The range and begin/end objects
remain in the enclosing loop scope.
Completed scalar assignments and supported library replacements restore state;
unknown calls do not prove reinitialization. Whole local objects and direct
local captures are tracked, while general memberwise moves in compound classes,
array-element move propagation, escaping lambdas or callbacks, and arbitrary
interprocedural effects remain outside the model. Selected returned-lambda
checks do not cover arbitrary uses after captured locals leave scope.

The check follows emitted CIR order, including explicitly sequenced operations,
short-circuit regions and supported temporary cleanup. Canonicalized CIR and
text/bytecode reloads use the same analysis. It does not explore every C++
evaluation order. A call such as `consume(*p, std::move(p))`, where the second
parameter is a `std::unique_ptr` passed by value, can therefore be missed when
the emitted order dereferences `p` before moving it. Another permitted ordering
moves first and then dereferences an empty pointer. This is an accepted gap
relative to `bugprone-use-after-move`; complete coverage parity is not promised.

The inherited aggregate pointer-field rules can produce conservative false
positives for compound operations. Custom tuple-like accessors do not acquire
the standard `std::get` contract through their name or structured-binding use.
Branch joins can lose correlations between conditions and object states,
retaining an invalid state that cannot reach a particular use. Conditional
transfers through unmodeled custom containers and guarded reuse after opaque
insertion calls can still produce false positives. Standard-library models cover
bounded contracts; unknown custom constructors cannot establish a library's
guaranteed post-move contents. See [CIR lifetime checking](https://clang.llvm.org/docs/CIR/LifetimeCheck.html)
for the analysis model and further limitations.

Diagnostic counts alone are not a measure of accuracy. In particular, scalar
move-intent and repeated-move policy warnings must be distinguished from actual
invalid uses when comparing this check with `bugprone-use-after-move`.
