# CIR lifetime checking

CIR provides an experimental, opt-in analysis of pointer and owner lifetimes.
It diagnoses supported uses of uninitialized, null, and dangling pointers,
owner invalidation, and returned lambdas that capture local variables by reference.
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
