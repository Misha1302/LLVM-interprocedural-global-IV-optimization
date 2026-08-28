# LLVM Interprocedural Global IV Optimization

This repository contains an LLVM pass that experiments with promoting simple integer global variables used as induction-like state inside loops to local stack storage.

The main goal is not to optimize every possible loop. The pass deliberately supports a narrow, well-defined subset of LLVM IR and rejects cases where preserving semantics is not obvious. For this project, a missed optimization is preferable to an incorrect transformation.

## What the pass does

For a supported loop, the pass can replace repeated accesses to a global integer with accesses to a local `alloca` and write the final value back to the global on loop exit.

Conceptually, a loop like this:

```llvm
%old = load i32, ptr @g
%next = add i32 %old, 1
store i32 %next, ptr @g
```

can be localized so the loop works with a local slot instead of touching `@g` on every iteration. If a known function called from the loop may read or write that global, the pass synchronizes the local value with the global around the call.

The analysis also tracks affine evolution of the form

```text
b + k * x    (mod 2^N)
```

using `llvm::APInt`, so normal fixed-width integer wraparound is modeled directly.

## Design

The implementation is split into several small components rather than keeping the whole pass in one file:

- `EvolutionAnalyzer` computes affine evolution for supported functions and loop iterations.
- `FunctionEvolutionState` stores the current interprocedural evolution state.
- `ModuleEffectsAnalyzer` collects direct and transitive reads, writes, and synchronization effects.
- `PromotionLegality` decides whether localization is safe for a particular loop/global pair.
- `LoopGlobalPromoter` performs the IR transformation after legality has already been established.
- `GlobalIVPass` only orchestrates analysis, legality checking, and transformation.
- `Plugin.cpp` registers the pass as `global-iv` for LLVM's new pass manager.

This separation is intentional: analysis may be conservative or incomplete without allowing transformation code to bypass the legality checks.

## Supported scope

The current implementation is an MVP and intentionally accepts only relatively simple cases. In particular, it focuses on directly trackable integer globals and innermost loops whose iteration path can be represented unambiguously.

The pass conservatively rejects or avoids transforming cases involving, among other things:

- volatile or atomic accesses;
- synchronization that could make localization unsafe;
- unknown or unsupported external/indirect calls;
- `returns_twice` or potentially throwing calls in the loop;
- unsupported control flow inside an iteration;
- missing loop preheaders;
- loops without a unique dedicated exit suitable for write-back;
- non-integer or otherwise non-trackable globals;
- non-affine and cross-global value dependencies that the current evolution model cannot prove safe.

Known direct calls are analyzed transitively. If a call may read the promoted global, the current local value is flushed before the call. If it may write the global, the value is reloaded afterwards as well.

## Requirements

The project is currently developed and tested against LLVM 22.1.8.

You need:

- CMake 3.20 or newer;
- a C++23-capable compiler;
- an LLVM installation exposing `llvm-config-22` or `llvm-config`;
- `opt` from the same LLVM installation;
- Python 3 for the regression runner;
- `clang` if you want the differential execution checks in the regression suite.

## Build

A clean out-of-tree build is recommended:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j
```

CMake discovers LLVM through `llvm-config --cmakedir` and builds the pass as an LLVM pass plugin named `GlobalIVPass`.

## Run the pass

The repository contains a smoke input and a CMake target for running it:

```bash
cmake --build build --target run-pass
```

The equivalent direct invocation is:

```bash
opt \
  -load-pass-plugin=build/GlobalIVPass.so \
  -passes=global-iv \
  -S tests/smoke.ll
```

The exact plugin path can depend on the generator and platform. If necessary, locate it with:

```bash
find build -name 'GlobalIVPass.so' -print
```

## Regression tests

The regression suite contains 29 LLVM IR cases covering both successful transformations and conservative rejection paths.

Run it with:

```bash
python3 tests/run_regressions.py \
  --plugin /path/to/GlobalIVPass.so \
  --llvm-root /path/to/llvm-22.1.8
```

The runner checks more than whether `opt` exits successfully:

1. every case is processed with `global-iv,verify`;
2. structural expectations are checked in the transformed IR;
3. a second application of the pass must reach the same canonical IR, which catches accidental repeated localization;
4. executable cases are compiled before and after transformation and their observable results are compared when `clang` is available.

A successful run ends with:

```text
passed=29 failed=0
```

The suite includes cases for basic affine updates, integer wraparound, multiple globals, direct and transitive calls, external calls, atomics, volatile accesses, branches, multiple exits, recursion, address spaces, alignment, `returns_twice`, early exits, non-linear expressions, cross-global dependencies, and loop-IV-dependent expressions.

See [`tests/README.md`](tests/README.md) for the suite-specific notes.

## Formatting

`apply-format.sh` contains the formatting and static cleanup rules used by the project.

To verify the current tree without modifying files:

```bash
./apply-format.sh --check
```

## Current status

This is a conservative prototype intended for review and further development, not a production replacement for LLVM's existing scalar-evolution or loop optimization infrastructure.

The useful property of the current version is its boundary: supported transformations are backed by explicit legality checks and regression counterexamples, while unsupported cases are left unchanged. Future work can expand that boundary without weakening the safety rules already in place.
