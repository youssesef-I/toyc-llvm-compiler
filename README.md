# ToyC: A Toy Optimizing Compiler with an LLVM Backend

ToyC is a small compiler for a C-like expression language written in C++. It includes a hand-written lexer, recursive-descent parser, AST, LLVM IR code generator, and a custom optimization pipeline using LLVM's New Pass Manager.

The main focus is a **constant-propagation pass implemented from scratch** as an iterative monotone dataflow analysis over a flat lattice. The front end intentionally emits simple memory-form LLVM IR, then LLVM's `mem2reg` promotes it to SSA so the custom analysis can reason about values.

Across a four-program benchmark suite, the pipeline reduces static LLVM IR instruction count from **70 instructions to 28**, a **60.0% reduction**.

---

## Build and run

Requires Clang, LLVM, and C++17. Developed against LLVM 21.

```sh
clang++ ToyC.cpp -o toy $(llvm-config --cxxflags --ldflags --libs all)
./toy
```

`--libs all` is the simple build option. Depending on the local LLVM installation, it can be narrowed to a smaller set of LLVM libraries to reduce link time.

Running `./toy` compiles the built-in benchmark programs, runs the optimization pipeline, and prints instruction counts after each pass.

---

## What is implemented

Implemented from scratch:

* Maximal-munch lexer
* Recursive-descent parser
* `std::unique_ptr`-owned AST
* LLVM IR code generation with `IRBuilder`
* Lowering for variables, assignment, arithmetic, comparisons, `if`/`else`, `while`, and `return`
* Constant propagation over a flat lattice
* Constant folding pass
* Benchmark harness for static IR instruction counts

Provided by LLVM:

* IR data structures
* `IRBuilder`
* New Pass Manager infrastructure
* `mem2reg`, used to promote memory-form IR into SSA form

---

## Results

Instruction counts are static LLVM IR instruction counts. Each column is cumulative: it shows the instruction count after that pass has run.

| program           | raw | mem2reg | prop | fold | reduction |
| ----------------- | --: | ------: | ---: | ---: | --------: |
| `arith_chain`     |  14 |       3 |    1 |    1 |     92.9% |
| `branch_agree`    |  17 |       8 |    7 |    7 |     58.8% |
| `branch_disagree` |  19 |      11 |    9 |    9 |     52.6% |
| `loop_sum`        |  20 |      11 |   11 |   11 |     45.0% |
| **TOTAL**         |  70 |      33 |   28 |   28 | **60.0%** |

This metric measures eliminated IR operations and code size, not wall-clock runtime. The project does not include a JIT or native-code benchmark.

### Reading the results

Most of the raw reduction comes from `mem2reg`. This is expected: the front end emits one `alloca` per variable, plus explicit `load` and `store` instructions, and `mem2reg` promotes that memory traffic into SSA registers.

The custom propagation pass performs the value reasoning after SSA construction. In `arith_chain`, it propagates constants through arithmetic and proves the function returns a single constant. In `branch_disagree`, it folds constants inside each branch but correctly refuses to fold the value after the merge, because the two incoming paths disagree. The meet of two distinct constants becomes `Bottom`, meaning "not a constant."

`branch_agree` also demonstrates an important ordering detail: an agreeing φ-node would meet to the same constant, but LLVM's `mem2reg` often removes trivial agreeing φ-nodes before the propagation pass sees them.

`loop_sum` shows the honest limit of the pass. The loop variables change across iterations, so their merged values are not constants. The reduction there comes entirely from `mem2reg`.

The `prop` and `fold` columns are identical on this benchmark suite because propagation already handles the cases that local constant folding would find. The folding pass is retained as a simpler baseline and cleanup pass.

---

## Language

ToyC supports a small C-like expression language:

* Numeric literals
* Variables and assignment
* `+`, `-`, `*`, `/`
* Comparisons
* `if`/`else`
* `while`
* `return`

Every value is a `double`; there is no separate type system.

Example:

```c
x = 10;
s = 0;

while (x > 0) {
    s = s + x;
    x = x - 1;
}

return s;
```

Operator precedence is encoded in the recursive-descent parser:

```text
comparison -> additive -> multiplicative -> factor
```

Comparisons lower to LLVM `fcmp` instructions and are converted back to `double`, keeping the source language uniformly numeric.

---

## Pipeline

```text
source -> lexer -> parser -> AST -> LLVM IR -> mem2reg -> const-prop -> const-fold -> LLVM IR
```

The front end emits naive memory-form LLVM IR. Each variable is allocated in the entry block, assignments become `store` instructions, and variable uses become `load` instructions.

Control flow is lowered manually into LLVM basic blocks:

* `if`/`else` becomes a condition, then block, else block, and merge block
* `while` becomes a condition block, loop body, and exit block, with a back-edge from the body to the condition

`mem2reg` is fixed at the front of the optimization pipeline because the custom propagation pass depends on SSA form. Before `mem2reg`, the pass mostly sees opaque loads and stores. After `mem2reg`, variables are SSA values and control-flow joins are represented by φ-nodes.

---

## Constant propagation

The propagation pass is a forward dataflow analysis followed by an IR rewrite.

Each SSA value is assigned an element of a flat lattice:

```text
Top        = unknown
Const(c)   = known compile-time constant c
Bottom     = not a constant / overdefined
```

The order is:

```text
Top > Const(c) > Bottom
```

Distinct constants are incomparable, so their meet is `Bottom`.

Meet combines information from multiple control-flow paths:

```text
Top ⊓ x = x
Bottom ⊓ x = Bottom
Const(c) ⊓ Const(c) = Const(c)
Const(c1) ⊓ Const(c2) = Bottom, when c1 != c2
```

Transfer functions model constants, binary operators, and φ-nodes. Unmodeled instructions conservatively produce `Bottom`.

The solver initializes values to `Top` and repeatedly applies transfer functions until no value changes. Since the lattice has finite height, each value can only move downward a bounded number of times:

```text
Top -> Const(c) -> Bottom
```

So the iteration terminates at a fixed point.

After solving, any SSA value proven constant is replaced with the corresponding LLVM `ConstantFP` using `replaceAllUsesWith`, and the old instruction is erased when safe.

This is classical constant propagation, not sparse conditional constant propagation. The pass propagates values, but it does not track basic-block reachability or delete unreachable branches.

---

## Constant folding

The constant-folding pass is a simpler local optimization. It replaces arithmetic operations whose operands are already literal constants.

Folding is less powerful than propagation because it only sees constants already present in the IR. Propagation can discover constants through SSA value chains and simple control-flow structure.

On the current benchmark suite, propagation subsumes folding, so the `prop` and `fold` instruction counts are identical.

---

## Limitations and future work

Current limitations:

* No control-flow simplification
* No sparse conditional constant propagation
* Round-robin fixed-point iteration instead of a def-use worklist
* No full dead-code elimination pass
* No loop-invariant code motion
* No user-defined functions
* A single numeric type: `double`
* Static instruction-count metric only, not runtime

Natural next steps:

* Add LLVM `simplifycfg` after propagation
* Implement sparse conditional constant propagation
* Replace round-robin iteration with a worklist solver
* Add dead-code elimination
* Extend the language with functions and multiple numeric types
* Add ORC JIT execution for runtime measurement

---

## Notes

Although ToyC is small, its structure mirrors larger compiler pipelines: source is lowered to LLVM IR, LLVM canonicalizes the program into SSA form, and custom passes perform dataflow analysis over that SSA representation.

The same analysis framework used here (lattice values, monotone transfer functions, and fixed-point iteration) also appears in liveness analysis, available expressions, and reaching definitions.

Because the code generator targets LLVM IR, backend-oriented extensions such as NVPTX lowering would be natural future work. The analysis itself is mostly independent of the final machine target.

