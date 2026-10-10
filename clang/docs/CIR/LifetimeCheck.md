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
without a live Clang AST. It follows emitted CIR operations and structured
regions; it does not construct or analyze an AST control-flow graph. Generate
such input with both
`-fclangir-lifetime-check -clangir-disable-passes`: the first option preserves
full-expression temporary scopes, while the second leaves the pass unrun. These
source lifetime boundaries are needed for temporary-dependent diagnostics,
including coroutine arguments. These additional full-expression scopes are
generated only when lifetime analysis is enabled.

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

The move checks track local scalar, pointer, owner, and whole class objects,
including reference parameters and local aliases. Assignments and supported
library replacements restore the destination after evaluating source uses.
Uncertain aliases retain all possible targets; writing through one does not
prove that every possible target was reinitialized.
By-value pointer-like record parameters arrive initialized. Their trivial
copies retain tracked borrow lifetimes and invalidation history; copying a
moved whole object also checks the source use.

For example, with history notes enabled:

```cpp
void consume(int &&);
int example() {
  int value = 42;
  consume(static_cast<int &&>(value)); // note: moved here via std::move or rvalue reference
  return value; // warning: use of moved-from value 'value'
}
```

This scalar example illustrates the checker's policy for an opaque call accepting
an rvalue reference. The reference binding itself does not change the integer.
A standalone rvalue cast or local reference binding does not consume a value in
the analysis. Repeated moves retain the existing diagnostic policy; a warning
is not, by itself, proof of undefined behavior.

Standard-library contracts distinguish initialized objects from usable contents.
For example, `unique_ptr::reset(new T)` restores usable contents, whereas
`reset()` leaves an initialized, empty pointer whose dereference is invalid.
Safe observers remain valid after moves and resets. Optional engagement is a
separate property from pointer nullness. The check also models supported weak
pointer, optional, any, container, tuple-reference assignment, and conditional
insertion operations. A generic `[[clang::reinitializes]]` contract applies on
normal return; an arbitrary mutable-reference call does not prove restoration.
These contracts use serialized declaration facts, not library implementation
bodies or clang-tidy-specific annotations.
The insertion-result contract maps the logical `pair::second` to its actual CIR
member, so implementation bases and padding do not change predicate tracking.
Verified `std::get` overloads for `std::pair` and `std::tuple` return element
references without consuming the containing object. Existing invalid-source
checks still apply. This contract does not provide precise element aliasing or
memberwise move analysis. For modeled library calls, unavailable reference
contents remain unknown; the argument's address is not treated as its contents.

Path state captures pointer values when they are evaluated, so later writes to
an alias do not change an earlier value snapshot. Structured conditionals, loop
fixed points, and normal, return, break, continue, and exceptional continuations
use that state. Direct local captures preserve their copy, alias, or transfer
behavior. General memberwise move analysis and escaping callbacks remain outside
this model. Selected aggregate pointer-field lifetime rules are retained and
can produce conservative diagnostics for compound operations. Custom tuple-like
accessors and opaque container calls do not acquire standard-library contracts
merely because they participate in structured bindings or accept an rvalue.

Range-for loop-variable and body-local storage has a separate lifetime for each
iteration. Its cleanup precedes the increment and runs on exits such as continue,
break, return, and exceptions. The range and begin/end objects remain in the
enclosing loop scope. Borrows of iteration-local objects become invalid when
that iteration ends.

The analysis follows the order chosen by CIR generation. It does not enumerate
other evaluation orders permitted by C++. For example:

```cpp
void consume(int, std::unique_ptr<int>);
void example(std::unique_ptr<int> p) {
  consume(*p, std::move(p));
}
```

Assuming `p` initially owns an integer, evaluating `*p` before moving `p` into the
second parameter is safe. Evaluating the second parameter first leaves `p`
empty before the dereference. The checker can miss this invalid use if CIR
emits the first ordering. It does not provide a general diagnosis of unsequenced
expressions or promise to cover every real issue detected by
`bugprone-use-after-move`.

Explicit sequencing, short-circuit regions and supported cleanup scopes are
analyzed in CIR. Serialized lifetime facts and compiler-generated value-spill
markers preserve the needed object and value information through
canonicalization and text/bytecode round trips. There is no source-order
scheduler or associated exploration budget.

Branch joins retain possible invalid states but can lose the relation between a
condition and another object's state. This can produce a false positive when
the invalid state cannot reach the reported use. The analysis does not provide
general path-feasibility proofs. Conditional transfers through unmodeled custom
containers and guarded reuse after opaque insertion calls can still produce
false positives.

Native CIR loops use the loop analysis described above. Jumps into nested
structured regions remain unsupported. Array constructor and destructor
regions are not generally modeled; some forms can pass without analyzing their
elements. General memberwise moves in compound classes, array-element move
propagation, and escaping lambdas or callbacks are deferred. The supported
returned-lambda and direct local-capture checks do not establish safety for
arbitrary uses after captured locals leave scope. A successful translation unit
therefore does not establish coverage of every operation.

Function metadata maps source parameters to CIR operands, including implicit
object and hidden arguments. Argument move tracking is unavailable where CIRGen
cannot prove that mapping. Arbitrary interprocedural effects and general
recursive field analysis are not provided.

CIR generation itself remains incomplete for some C++ constructs. A generation,
verification, or pass error is an analysis failure, not evidence that the input
is safe. Keep IR verification enabled when evaluating the checker.
