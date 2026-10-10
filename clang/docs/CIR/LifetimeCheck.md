# CIR lifetime checking

CIR provides an experimental, opt-in analysis of pointer and owner lifetimes.
It diagnoses supported uses of uninitialized, null, and dangling pointers,
owner invalidation, returned lambdas that capture local variables by reference,
and uses of tracked values after a move.
It is not a complete proof of lifetime safety.

Build Clang with `CLANG_ENABLE_CIR=ON` and LLVM projects `clang;mlir`. For example:

```sh
clang++ -fclangir -fclangir-lifetime-check -c example.cpp
clang++ -fclangir '-fclangir-lifetime-check=history=all;history_limit=2' -c example.cpp
```

The pass runs after CIR canonicalization and before simplification, library
optimizations, ABI conversion, or control-flow lowering. It does not change the
program. Record and function semantics are serialized in CIR attributes, so the
analysis can also run through `cir-opt --cir-lifetime-check` on structured CIR
without a live Clang AST. Generate such input with both
`-fclangir-lifetime-check -clangir-disable-passes`: the first option preserves
full-expression temporary scopes, while the second leaves the pass unrun. These
source lifetime boundaries are needed for temporary-dependent diagnostics,
including coroutine arguments. Ordinary compilation keeps its existing scopes.

## Options

Options in `-fclangir-lifetime-check=...` are separated by semicolons or spaces.
List values are comma separated. Clang remarks also require `-Rremark-clangir`; this
uses the existing ClangIR diagnostic group.

| Option | Values | Default |
|---|---|---|
| `history` | `all`, `null`, `invalid` | No history notes |
| `history_limit` | Nonnegative integer | `1` |
| `remarks` | `all`, `pset-invalid`, `pset-always` | No remarks |

## Coverage and limitations

The move checks track arguments passed to rvalue-reference parameters, scalar
initialization from moved values, and move construction and assignment of
supported owner and pointer types. Function metadata maps source parameters to
CIR operands, including the implicit object argument of an instance method.
The checker skips argument-based move tracking when that mapping is unavailable.

For example, with history notes enabled:

```cpp
void consume(int &&);
int example() {
  int value = 42;
  consume(static_cast<int &&>(value)); // note: moved here via std::move or rvalue reference
  return value; // warning: use of moved-from value 'value'
}
```

For recognized `std::unique_ptr` and `std::shared_ptr` owners, dereferencing a
moved-from object is diagnosed. Calls to `get`, `release`, `reset`, and
`operator bool` are treated as safe observations or operations. Calling
`reset(new T)` does not yet restore the tracked state for later dereferences.

This reviewed move model has known precision limits. A plain scalar copy into
another local can incorrectly mark the source as moved-from, and assignment
does not fully model reinitialization. Temporary suppression uses the existing
`ref.tmp` naming convention and applies only to the reviewed move paths.

The analysis tracks local pointer and owner state through supported structured
scopes, branches, switches, and a bounded loop approximation. It recognizes
explicit and Clang-inferred lifetime attributes. Aggregate support is limited
to selected pointer fields; it is not general recursive field analysis. Passing
an aggregate with a null pointer field to an implicit assignment can produce
a conservative false positive even when that field is only copied.

Cleanup scopes include their body and applicable normal cleanup. Exception-only
cleanup is not treated as normal execution. General exception-flow analysis and
conditional-expression regions are not modeled. Other limitations include
indirect/interprocedural effects, arbitrary lambda escapes, and moves or lifetime
effects through complex aliases and nested members.

CIR generation itself remains incomplete for some C++ constructs. A generation,
verification, or pass error is an analysis failure, not evidence that the input
is safe. Keep IR verification enabled when evaluating the checker.
